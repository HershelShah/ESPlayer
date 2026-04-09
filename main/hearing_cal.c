#include "hearing_cal.h"
#include "config.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "hearing_cal";

// Test frequencies: covers the range most affected by hearing loss
const float CAL_TEST_FREQS[CAL_NUM_FREQS] = {
    250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 6000.0f, 8000.0f, 12000.0f
};

static cal_status_t s_status;
static float        s_phase;
static int64_t      s_pause_until;  // Timestamp for pause between tones

// Volume ramp parameters
// The generate function is called ~350 times/sec (every ~3ms from BT callback).
// We want the ramp from -54dB to -6dB to take ~15 seconds.
// That's 48dB over 15s = 3.2 dB/sec = 0.009 dB per call.
// In linear: 10^(0.009/20) ≈ 1.001
#define VOL_START     0.002f   // Start very quiet (-54 dB)
#define VOL_MAX       0.5f     // Max ramp level (-6 dB)
#define VOL_STEP      1.001f   // ~3.2 dB/sec ramp — 15 seconds to full
#define PAUSE_MS      1200     // Pause between frequencies

void hearing_cal_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = CAL_STATE_IDLE;
    s_phase = 0.0f;
}

void hearing_cal_start(void)
{
    ESP_LOGI(TAG, "Starting hearing calibration (%d frequencies)", CAL_NUM_FREQS);
    memset(&s_status, 0, sizeof(s_status));
    s_status.state = CAL_STATE_PLAYING;
    s_status.current_freq_idx = 0;
    s_status.current_volume = VOL_START;
    s_phase = 0.0f;
}

void hearing_cal_confirm(void)
{
    if (s_status.state != CAL_STATE_PLAYING) return;

    int idx = s_status.current_freq_idx;

    // Record threshold as relative dB from the starting level
    // Lower current_volume at confirmation = better hearing at this freq
    float threshold_db = 20.0f * log10f(s_status.current_volume / VOL_START);
    s_status.thresholds[idx] = threshold_db;
    s_status.freq_done[idx] = true;

    ESP_LOGI(TAG, "Freq %.0f Hz: heard at volume %.4f (%.1f dB from start)",
             CAL_TEST_FREQS[idx], s_status.current_volume, threshold_db);

    // Move to next frequency or finish
    s_status.current_freq_idx++;
    if (s_status.current_freq_idx >= CAL_NUM_FREQS) {
        s_status.state = CAL_STATE_DONE;
        ESP_LOGI(TAG, "Calibration complete!");
    } else {
        // Brief pause before next tone
        s_status.state = CAL_STATE_PAUSE;
        s_pause_until = esp_timer_get_time() + (PAUSE_MS * 1000LL);
        s_status.current_volume = VOL_START;
        s_phase = 0.0f;
    }
}

bool hearing_cal_generate(int16_t *buf, int frame_count, int sample_rate)
{
    if (s_status.state == CAL_STATE_IDLE || s_status.state == CAL_STATE_DONE) {
        return false;
    }

    if (s_status.state == CAL_STATE_PAUSE) {
        // Silence during pause
        memset(buf, 0, frame_count * 4);
        if (esp_timer_get_time() >= s_pause_until) {
            s_status.state = CAL_STATE_PLAYING;
        }
        return true;
    }

    // CAL_STATE_PLAYING: generate sine tone at current frequency and volume
    float freq = CAL_TEST_FREQS[s_status.current_freq_idx];
    float vol = s_status.current_volume;
    float phase_inc = 2.0f * M_PI * freq / (float)sample_rate;

    for (int i = 0; i < frame_count; i++) {
        float sample = sinf(s_phase) * vol * 32000.0f;
        int16_t val = (int16_t)sample;
        buf[i * 2]     = val;  // Left
        buf[i * 2 + 1] = val;  // Right
        s_phase += phase_inc;
        if (s_phase >= 2.0f * M_PI) s_phase -= 2.0f * M_PI;
    }

    // Ramp volume up
    s_status.current_volume *= VOL_STEP;
    if (s_status.current_volume > VOL_MAX) {
        s_status.current_volume = VOL_MAX;
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

    eq_profile_t profile;
    audio_eq_build_hearing_profile(CAL_TEST_FREQS, s_status.thresholds,
                                   CAL_NUM_FREQS, &profile);
    audio_eq_load_profile(&profile);
    ESP_LOGI(TAG, "Applied hearing correction profile");
}

void hearing_cal_save(void)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/eq_profiles/hearing.txt", SD_MOUNT_POINT);

    // First make sure directory exists
    char dir[48];
    snprintf(dir, sizeof(dir), "%s/eq_profiles", SD_MOUNT_POINT);
    mkdir(dir, 0755);

    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot save hearing data to %s", path);
        return;
    }

    fprintf(f, "# Hearing calibration results\n");
    for (int i = 0; i < CAL_NUM_FREQS; i++) {
        fprintf(f, "%.0f,%.2f\n", CAL_TEST_FREQS[i], s_status.thresholds[i]);
    }

    fclose(f);

    // Also save as EQ profile
    audio_eq_save_profile("hearing.txt");
    ESP_LOGI(TAG, "Saved hearing calibration to SD card");
}
