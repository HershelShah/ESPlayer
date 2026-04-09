#include "audio_eq.h"
#include "config.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

#include "esp_log.h"

static const char *TAG = "eq";

// ---------------------------------------------------------------------------
// Biquad filter (Direct Form II Transposed)
// ---------------------------------------------------------------------------
typedef struct {
    float b0, b1, b2;
    float a1, a2;
    float z1[2], z2[2];  // State per stereo channel
} biquad_t;

static biquad_t    s_filters[EQ_MAX_BANDS];
static eq_profile_t s_profile;
static int          s_sample_rate;
static bool         s_bypass = false;

// ---------------------------------------------------------------------------
// Coefficient computation (Bristow-Johnson Audio EQ Cookbook)
// ---------------------------------------------------------------------------

static void compute_peaking(biquad_t *bq, float fs, float f0, float gain_db, float Q)
{
    float A  = powf(10.0f, gain_db / 20.0f);
    float w0 = 2.0f * M_PI * f0 / fs;
    float cw = cosf(w0);
    float alpha = sinf(w0) / (2.0f * Q);

    float a0 = 1.0f + alpha / A;
    bq->b0 = (1.0f + alpha * A) / a0;
    bq->b1 = (-2.0f * cw)       / a0;
    bq->b2 = (1.0f - alpha * A) / a0;
    bq->a1 = (-2.0f * cw)       / a0;
    bq->a2 = (1.0f - alpha / A) / a0;
}

static void compute_low_shelf(biquad_t *bq, float fs, float f0, float gain_db, float Q)
{
    float A   = powf(10.0f, gain_db / 40.0f);
    float w0  = 2.0f * M_PI * f0 / fs;
    float cw  = cosf(w0);
    float sw  = sinf(w0);
    float alpha = sw / (2.0f * Q);
    float sqA = sqrtf(A);

    float a0 =        (A + 1) + (A - 1) * cw + 2 * sqA * alpha;
    bq->b0 =     A * ((A + 1) - (A - 1) * cw + 2 * sqA * alpha) / a0;
    bq->b1 = 2 * A * ((A - 1) - (A + 1) * cw                  ) / a0;
    bq->b2 =     A * ((A + 1) - (A - 1) * cw - 2 * sqA * alpha) / a0;
    bq->a1 =    -2 * ((A - 1) + (A + 1) * cw                  ) / a0;
    bq->a2 =         ((A + 1) + (A - 1) * cw - 2 * sqA * alpha) / a0;
}

static void compute_high_shelf(biquad_t *bq, float fs, float f0, float gain_db, float Q)
{
    float A   = powf(10.0f, gain_db / 40.0f);
    float w0  = 2.0f * M_PI * f0 / fs;
    float cw  = cosf(w0);
    float sw  = sinf(w0);
    float alpha = sw / (2.0f * Q);
    float sqA = sqrtf(A);

    float a0 =        (A + 1) - (A - 1) * cw + 2 * sqA * alpha;
    bq->b0 =     A * ((A + 1) + (A - 1) * cw + 2 * sqA * alpha) / a0;
    bq->b1 =-2 * A * ((A - 1) + (A + 1) * cw                  ) / a0;
    bq->b2 =     A * ((A + 1) + (A - 1) * cw - 2 * sqA * alpha) / a0;
    bq->a1 =     2 * ((A - 1) - (A + 1) * cw                  ) / a0;
    bq->a2 =         ((A + 1) - (A - 1) * cw - 2 * sqA * alpha) / a0;
}

static void recompute_filter(int idx)
{
    eq_band_config_t *cfg = &s_profile.bands[idx];
    biquad_t *bq = &s_filters[idx];

    if (!cfg->enabled || cfg->gain_db == 0.0f) {
        // Unity: b0=1, everything else 0
        bq->b0 = 1.0f; bq->b1 = 0; bq->b2 = 0;
        bq->a1 = 0;    bq->a2 = 0;
        return;
    }

    float fs = (float)s_sample_rate;
    switch (cfg->type) {
    case EQ_FILTER_LOW_SHELF:
        compute_low_shelf(bq, fs, cfg->freq, cfg->gain_db, cfg->q);
        break;
    case EQ_FILTER_HIGH_SHELF:
        compute_high_shelf(bq, fs, cfg->freq, cfg->gain_db, cfg->q);
        break;
    case EQ_FILTER_PEAKING:
    default:
        compute_peaking(bq, fs, cfg->freq, cfg->gain_db, cfg->q);
        break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void audio_eq_init(int sample_rate)
{
    s_sample_rate = sample_rate;
    memset(s_filters, 0, sizeof(s_filters));
    audio_eq_preset_flat();
    ESP_LOGI(TAG, "EQ init: %d Hz, %d bands max", sample_rate, EQ_MAX_BANDS);
}

void audio_eq_load_profile(const eq_profile_t *profile)
{
    memcpy(&s_profile, profile, sizeof(eq_profile_t));
    // Reset all filter states
    for (int i = 0; i < EQ_MAX_BANDS; i++) {
        s_filters[i].z1[0] = s_filters[i].z1[1] = 0;
        s_filters[i].z2[0] = s_filters[i].z2[1] = 0;
        recompute_filter(i);
    }
    ESP_LOGI(TAG, "Loaded profile: %s (%d bands)", s_profile.name, s_profile.band_count);
}

const eq_profile_t *audio_eq_get_profile(void)
{
    return &s_profile;
}

void audio_eq_set_band_gain(int band_index, float gain_db)
{
    if (band_index < 0 || band_index >= s_profile.band_count) return;
    if (gain_db < -12.0f) gain_db = -12.0f;
    if (gain_db >  12.0f) gain_db =  12.0f;
    s_profile.bands[band_index].gain_db = gain_db;
    s_filters[band_index].z1[0] = s_filters[band_index].z1[1] = 0;
    s_filters[band_index].z2[0] = s_filters[band_index].z2[1] = 0;
    recompute_filter(band_index);
}

void audio_eq_set_bypass(bool bypass)
{
    s_bypass = bypass;
    ESP_LOGI(TAG, "EQ bypass: %s", bypass ? "ON" : "OFF");
}

bool audio_eq_get_bypass(void)
{
    return s_bypass;
}

void audio_eq_process(int16_t *samples, int frame_count)
{
    if (s_bypass || s_profile.band_count == 0) return;

    for (int b = 0; b < s_profile.band_count; b++) {
        if (!s_profile.bands[b].enabled || s_profile.bands[b].gain_db == 0.0f)
            continue;

        biquad_t *bq = &s_filters[b];

        for (int ch = 0; ch < 2; ch++) {
            float z1 = bq->z1[ch];
            float z2 = bq->z2[ch];

            for (int i = ch; i < frame_count * 2; i += 2) {
                float x = (float)samples[i];
                float y = bq->b0 * x + z1;
                z1 = bq->b1 * x - bq->a1 * y + z2;
                z2 = bq->b2 * x - bq->a2 * y;
                if (y > 32767.0f)  y = 32767.0f;
                if (y < -32768.0f) y = -32768.0f;
                samples[i] = (int16_t)y;
            }

            bq->z1[ch] = z1;
            bq->z2[ch] = z2;
        }
    }
}

// ---------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------

void audio_eq_preset_flat(void)
{
    memset(&s_profile, 0, sizeof(s_profile));
    strncpy(s_profile.name, "Flat", sizeof(s_profile.name));
    s_profile.band_count = 0;
}

void audio_eq_preset_edm(void)
{
    memset(&s_profile, 0, sizeof(s_profile));
    strncpy(s_profile.name, "EDM", sizeof(s_profile.name));
    s_profile.band_count = 5;

    // Sub-bass boost
    s_profile.bands[0] = (eq_band_config_t){ .freq = 60,   .gain_db = 5.0f,  .q = 0.7f, .type = EQ_FILTER_LOW_SHELF, .enabled = true };
    // Bass punch
    s_profile.bands[1] = (eq_band_config_t){ .freq = 150,  .gain_db = 3.0f,  .q = 1.0f, .type = EQ_FILTER_PEAKING,   .enabled = true };
    // Mid scoop
    s_profile.bands[2] = (eq_band_config_t){ .freq = 800,  .gain_db = -3.0f, .q = 1.0f, .type = EQ_FILTER_PEAKING,   .enabled = true };
    // Presence
    s_profile.bands[3] = (eq_band_config_t){ .freq = 3000, .gain_db = 2.0f,  .q = 1.0f, .type = EQ_FILTER_PEAKING,   .enabled = true };
    // Air / treble
    s_profile.bands[4] = (eq_band_config_t){ .freq = 10000, .gain_db = 4.0f, .q = 0.7f, .type = EQ_FILTER_HIGH_SHELF, .enabled = true };

    for (int i = 0; i < s_profile.band_count; i++)
        recompute_filter(i);

    ESP_LOGI(TAG, "Loaded EDM preset");
}

// ---------------------------------------------------------------------------
// Save / Load profiles to SD card
// ---------------------------------------------------------------------------

esp_err_t audio_eq_save_profile(const char *filename)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/eq_profiles/%s", SD_MOUNT_POINT, filename);

    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot write %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    fprintf(f, "name=%s\n", s_profile.name);
    fprintf(f, "bands=%d\n", s_profile.band_count);
    for (int i = 0; i < s_profile.band_count; i++) {
        eq_band_config_t *b = &s_profile.bands[i];
        fprintf(f, "band=%d,%.1f,%.2f,%.2f,%d,%d\n",
                i, b->freq, b->gain_db, b->q, b->type, b->enabled);
    }

    fclose(f);
    ESP_LOGI(TAG, "Saved profile to %s", path);
    return ESP_OK;
}

esp_err_t audio_eq_load_profile_file(const char *filename)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/eq_profiles/%s", SD_MOUNT_POINT, filename);

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Cannot read %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    eq_profile_t prof;
    memset(&prof, 0, sizeof(prof));

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "name=", 5) == 0) {
            sscanf(line + 5, "%31s", prof.name);
        } else if (strncmp(line, "bands=", 6) == 0) {
            sscanf(line + 6, "%d", &prof.band_count);
            if (prof.band_count > EQ_MAX_BANDS) prof.band_count = EQ_MAX_BANDS;
        } else if (strncmp(line, "band=", 5) == 0) {
            int idx, type, enabled;
            float freq, gain, q;
            if (sscanf(line + 5, "%d,%f,%f,%f,%d,%d", &idx, &freq, &gain, &q, &type, &enabled) == 6) {
                if (idx >= 0 && idx < EQ_MAX_BANDS) {
                    prof.bands[idx].freq    = freq;
                    prof.bands[idx].gain_db = gain;
                    prof.bands[idx].q       = q;
                    prof.bands[idx].type    = (eq_filter_type_t)type;
                    prof.bands[idx].enabled = (bool)enabled;
                }
            }
        }
    }

    fclose(f);
    audio_eq_load_profile(&prof);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Hearing correction profile builder
// ---------------------------------------------------------------------------

void audio_eq_build_hearing_profile(const float *test_freqs, const float *thresholds,
                                    int n, eq_profile_t *out)
{
    memset(out, 0, sizeof(eq_profile_t));
    strncpy(out->name, "Hearing", sizeof(out->name));

    // Reference: "normal" hearing threshold at each frequency.
    // Based on ISO 226 equal-loudness contour at threshold of hearing.
    // Approximate values in dB SPL for young adult with normal hearing.
    static const float ref_thresholds[] = {
        // 250Hz, 500Hz, 1kHz, 2kHz, 4kHz, 6kHz, 8kHz, 12kHz
        10.0f, 4.0f, 2.0f, -1.0f, -4.0f, 2.0f, 10.0f, 30.0f
    };

    // For each test frequency, compute how much the user deviates from reference.
    // If user needs more volume than reference → they're less sensitive → boost.
    out->band_count = (n > EQ_MAX_BANDS) ? EQ_MAX_BANDS : n;

    // Find the user's best (lowest) threshold to use as anchor
    float best_threshold = thresholds[0];
    float best_ref = ref_thresholds[0];
    for (int i = 1; i < n && i < 8; i++) {
        if (thresholds[i] - (i < 8 ? ref_thresholds[i] : ref_thresholds[7]) <
            best_threshold - best_ref) {
            best_threshold = thresholds[i];
            best_ref = (i < 8) ? ref_thresholds[i] : ref_thresholds[7];
        }
    }
    float anchor = best_threshold - best_ref;

    for (int i = 0; i < out->band_count; i++) {
        float ref = (i < 8) ? ref_thresholds[i] : ref_thresholds[7];
        // Deviation: how much worse is the user vs reference (anchored)
        float deviation = (thresholds[i] - ref) - anchor;

        // Positive deviation = user needs more volume = boost that freq
        // Clamp to reasonable range
        float gain = deviation;
        if (gain < -12.0f) gain = -12.0f;
        if (gain >  12.0f) gain =  12.0f;

        out->bands[i].freq    = test_freqs[i];
        out->bands[i].gain_db = gain;
        out->bands[i].q       = 1.5f;  // Moderate Q for smooth correction
        out->bands[i].type    = EQ_FILTER_PEAKING;
        out->bands[i].enabled = (fabsf(gain) > 0.5f);  // Skip negligible corrections

        ESP_LOGI(TAG, "Hearing band %d: %.0f Hz, threshold=%.1f dB, ref=%.1f, correction=%+.1f dB",
                 i, test_freqs[i], thresholds[i], ref, gain);
    }
}
