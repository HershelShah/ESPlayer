#include "hearing_cal.h"
#include "config.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "hearing_cal";

// 7 test frequencies: 250Hz to 8kHz (12kHz dropped — SBC codec corrupts it)
const float CAL_TEST_FREQS[CAL_NUM_FREQS] = {
    250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 6000.0f, 8000.0f
};

static cal_status_t s_status;
static float        s_phase;
static int64_t      s_pause_until;
static int64_t      s_tone_start;     // When current tone presentation started

// Adaptive staircase parameters
#define INITIAL_DB      -54.0f   // Start level (very quiet)
#define MAX_DB          -6.0f    // Max level
#define STEP_LARGE      6        // Initial step (dB)
#define STEP_SMALL      3        // Step after first reversal (dB)
#define TARGET_REVERSALS 6       // Stop after this many reversals
#define TONE_DURATION_MS 2000    // How long to present each tone
#define PAUSE_MS         800     // Silence between presentations

// Convert dB to linear amplitude
static float db_to_linear(float db)
{
    return powf(10.0f, db / 20.0f);
}

void hearing_cal_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = CAL_STATE_IDLE;
    s_phase = 0.0f;
}

static void start_frequency(void)
{
    s_status.current_volume_db = INITIAL_DB;
    s_status.reversals = 0;
    s_status.last_direction = 1;  // ascending
    s_status.step_db = STEP_LARGE;
    s_status.user_responded = false;
    s_status.state = CAL_STATE_PLAYING;
    s_tone_start = esp_timer_get_time();
    s_phase = 0.0f;
    memset(s_status.reversal_levels, 0, sizeof(s_status.reversal_levels));

    ESP_LOGI(TAG, "Testing %s ear, %.0f Hz",
             s_status.current_ear == CAL_EAR_LEFT ? "LEFT" : "RIGHT",
             CAL_TEST_FREQS[s_status.current_freq_idx]);
}

static void advance_to_next(void)
{
    // Record threshold as average of last 4 reversals (standard practice)
    float sum = 0;
    int count = 0;
    int start = (s_status.reversals > 4) ? s_status.reversals - 4 : 0;
    for (int i = start; i < s_status.reversals; i++) {
        sum += s_status.reversal_levels[i];
        count++;
    }
    float threshold = (count > 0) ? sum / count : s_status.current_volume_db;

    int idx = s_status.current_freq_idx;
    if (s_status.current_ear == CAL_EAR_LEFT) {
        s_status.thresholds_l[idx] = threshold;
    } else {
        s_status.thresholds_r[idx] = threshold;
    }
    s_status.freq_done[idx] = true;

    ESP_LOGI(TAG, "  Threshold: %.1f dB (%d reversals)",
             threshold, s_status.reversals);

    // Move to next frequency or next ear or finish
    s_status.current_freq_idx++;
    if (s_status.current_freq_idx >= CAL_NUM_FREQS) {
        if (s_status.current_ear == CAL_EAR_LEFT) {
            // Switch to right ear
            s_status.current_ear = CAL_EAR_RIGHT;
            s_status.current_freq_idx = 0;
            memset(s_status.freq_done, 0, sizeof(s_status.freq_done));
            s_status.state = CAL_STATE_PAUSE;
            s_pause_until = esp_timer_get_time() + (2000000LL);  // 2s pause between ears
            ESP_LOGI(TAG, "Left ear done. Switching to right ear...");
        } else {
            // Both ears done
            s_status.state = CAL_STATE_DONE;
            ESP_LOGI(TAG, "Calibration complete!");
        }
    } else {
        s_status.state = CAL_STATE_PAUSE;
        s_pause_until = esp_timer_get_time() + (PAUSE_MS * 1000LL);
    }
}

static void step_staircase(bool heard)
{
    int new_direction;
    if (heard) {
        // User heard it → decrease level (make it harder)
        new_direction = -1;
        s_status.current_volume_db -= s_status.step_db;
        if (s_status.current_volume_db < INITIAL_DB)
            s_status.current_volume_db = INITIAL_DB;
    } else {
        // User didn't hear it → increase level (make it easier)
        new_direction = 1;
        s_status.current_volume_db += s_status.step_db;
        if (s_status.current_volume_db > MAX_DB)
            s_status.current_volume_db = MAX_DB;
    }

    // Check for reversal (direction change)
    if (new_direction != s_status.last_direction && s_status.last_direction != 0) {
        if (s_status.reversals < 8) {
            s_status.reversal_levels[s_status.reversals] = s_status.current_volume_db;
        }
        s_status.reversals++;

        // After first reversal, switch to smaller steps
        if (s_status.reversals == 1) {
            s_status.step_db = STEP_SMALL;
        }

        ESP_LOGI(TAG, "  Reversal %d at %.1f dB (step=%d)",
                 s_status.reversals, s_status.current_volume_db, s_status.step_db);

        // Done with this frequency after enough reversals
        if (s_status.reversals >= TARGET_REVERSALS) {
            advance_to_next();
            return;
        }
    }
    s_status.last_direction = new_direction;

    // Present next tone
    s_status.user_responded = false;
    s_status.state = CAL_STATE_PAUSE;
    s_pause_until = esp_timer_get_time() + (PAUSE_MS * 1000LL);
}

void hearing_cal_start(void)
{
    ESP_LOGI(TAG, "Starting hearing calibration (%d freqs, L+R ears)", CAL_NUM_FREQS);
    memset(&s_status, 0, sizeof(s_status));
    s_status.current_ear = CAL_EAR_LEFT;
    s_status.current_freq_idx = 0;
    start_frequency();
}

void hearing_cal_confirm(void)
{
    if (s_status.state != CAL_STATE_PLAYING) return;
    s_status.user_responded = true;
    step_staircase(true);  // User heard the tone
}

void hearing_cal_no_response(void)
{
    if (s_status.state != CAL_STATE_PLAYING) return;
    step_staircase(false);  // User didn't hear the tone
}

bool hearing_cal_generate(int16_t *buf, int frame_count, int sample_rate)
{
    if (s_status.state == CAL_STATE_IDLE || s_status.state == CAL_STATE_DONE) {
        return false;
    }

    if (s_status.state == CAL_STATE_PAUSE) {
        memset(buf, 0, frame_count * 4);
        if (esp_timer_get_time() >= s_pause_until) {
            if (s_status.current_freq_idx < CAL_NUM_FREQS) {
                if (!s_status.freq_done[s_status.current_freq_idx] ||
                    s_status.reversals < TARGET_REVERSALS) {
                    s_status.state = CAL_STATE_PLAYING;
                    s_tone_start = esp_timer_get_time();
                    s_status.user_responded = false;
                } else {
                    start_frequency();
                }
            }
        }
        return true;
    }

    // CAL_STATE_PLAYING: generate sine tone
    float freq = CAL_TEST_FREQS[s_status.current_freq_idx];
    float vol = db_to_linear(s_status.current_volume_db);
    float phase_inc = 2.0f * M_PI * freq / (float)sample_rate;

    for (int i = 0; i < frame_count; i++) {
        float sample = sinf(s_phase) * vol * 32000.0f;
        int16_t val = (int16_t)sample;

        // Per-ear presentation: only play in the ear being tested
        if (s_status.current_ear == CAL_EAR_LEFT) {
            buf[i * 2]     = val;  // Left
            buf[i * 2 + 1] = 0;   // Right silent
        } else {
            buf[i * 2]     = 0;   // Left silent
            buf[i * 2 + 1] = val;  // Right
        }

        s_phase += phase_inc;
        if (s_phase >= 2.0f * M_PI) s_phase -= 2.0f * M_PI;
    }

    // Auto-timeout: if tone has played for TONE_DURATION_MS with no response,
    // treat as "didn't hear it"
    int64_t elapsed_us = esp_timer_get_time() - s_tone_start;
    if (elapsed_us > (TONE_DURATION_MS * 1000LL) && !s_status.user_responded) {
        ESP_LOGI(TAG, "  No response at %.1f dB — increasing",
                 s_status.current_volume_db);
        step_staircase(false);
    }

    return true;
}

const cal_status_t *hearing_cal_get_status(void)
{
    return &s_status;
}

void hearing_cal_apply(void)
{
    if (s_status.state != CAL_STATE_DONE) {
        ESP_LOGW(TAG, "Calibration not complete");
        return;
    }

    // Average L+R thresholds for the correction profile
    float avg_thresholds[CAL_NUM_FREQS];
    for (int i = 0; i < CAL_NUM_FREQS; i++) {
        avg_thresholds[i] = (s_status.thresholds_l[i] + s_status.thresholds_r[i]) / 2.0f;
    }

    eq_profile_t profile;
    audio_eq_build_hearing_profile(CAL_TEST_FREQS, avg_thresholds,
                                   CAL_NUM_FREQS, &profile);
    audio_eq_load_profile(&profile);
    ESP_LOGI(TAG, "Applied hearing correction (L+R averaged)");
}

void hearing_cal_save(void)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/eq_profiles/hearing.txt", SD_MOUNT_POINT);

    char dir[48];
    snprintf(dir, sizeof(dir), "%s/eq_profiles", SD_MOUNT_POINT);
    mkdir(dir, 0755);

    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot save to %s", path);
        return;
    }

    fprintf(f, "# Hearing calibration (adaptive staircase, per-ear)\n");
    fprintf(f, "# freq_hz,left_db,right_db\n");
    for (int i = 0; i < CAL_NUM_FREQS; i++) {
        fprintf(f, "%.0f,%.2f,%.2f\n",
                CAL_TEST_FREQS[i],
                s_status.thresholds_l[i],
                s_status.thresholds_r[i]);
    }

    fclose(f);
    audio_eq_save_profile("hearing.txt");
    ESP_LOGI(TAG, "Saved hearing calibration to SD card");
}
