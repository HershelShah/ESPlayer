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

// Coefficient computation in DOUBLE precision, stored as float.
// Double precision eliminates rounding errors for low-frequency filters
// (e.g. 60Hz shelf at 44100Hz where cos(w0) ≈ 0.99996).

static void compute_peaking(biquad_t *bq, float fs, float f0, float gain_db, float Q)
{
    double A  = pow(10.0, (double)gain_db / 20.0);
    double w0 = 2.0 * M_PI * (double)f0 / (double)fs;
    double cw = cos(w0);
    double alpha = sin(w0) / (2.0 * (double)Q);

    double a0 = 1.0 + alpha / A;
    bq->b0 = (float)((1.0 + alpha * A) / a0);
    bq->b1 = (float)((-2.0 * cw)       / a0);
    bq->b2 = (float)((1.0 - alpha * A) / a0);
    bq->a1 = (float)((-2.0 * cw)       / a0);
    bq->a2 = (float)((1.0 - alpha / A) / a0);
}

static void compute_low_shelf(biquad_t *bq, float fs, float f0, float gain_db, float Q)
{
    double A   = pow(10.0, (double)gain_db / 40.0);
    double w0  = 2.0 * M_PI * (double)f0 / (double)fs;
    double cw  = cos(w0);
    double sw  = sin(w0);
    double alpha = sw / (2.0 * (double)Q);
    double sqA = sqrt(A);

    double a0 =        (A + 1) + (A - 1) * cw + 2 * sqA * alpha;
    bq->b0 = (float)(     A * ((A + 1) - (A - 1) * cw + 2 * sqA * alpha) / a0);
    bq->b1 = (float)( 2 * A * ((A - 1) - (A + 1) * cw                  ) / a0);
    bq->b2 = (float)(     A * ((A + 1) - (A - 1) * cw - 2 * sqA * alpha) / a0);
    bq->a1 = (float)(    -2 * ((A - 1) + (A + 1) * cw                  ) / a0);
    bq->a2 = (float)(         ((A + 1) + (A - 1) * cw - 2 * sqA * alpha) / a0);
}

static void compute_high_shelf(biquad_t *bq, float fs, float f0, float gain_db, float Q)
{
    double A   = pow(10.0, (double)gain_db / 40.0);
    double w0  = 2.0 * M_PI * (double)f0 / (double)fs;
    double cw  = cos(w0);
    double sw  = sin(w0);
    double alpha = sw / (2.0 * (double)Q);
    double sqA = sqrt(A);

    double a0 =        (A + 1) - (A - 1) * cw + 2 * sqA * alpha;
    bq->b0 = (float)(     A * ((A + 1) + (A - 1) * cw + 2 * sqA * alpha) / a0);
    bq->b1 = (float)(-2 * A * ((A - 1) + (A + 1) * cw                  ) / a0);
    bq->b2 = (float)(     A * ((A + 1) + (A - 1) * cw - 2 * sqA * alpha) / a0);
    bq->a1 = (float)(     2 * ((A - 1) - (A + 1) * cw                  ) / a0);
    bq->a2 = (float)(         ((A + 1) - (A - 1) * cw - 2 * sqA * alpha) / a0);
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
    // Do NOT reset z1/z2 — let filter state decay naturally to avoid clicks
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

    int total = frame_count * 2;

    // Process per-sample in float: int16 → float → all biquads → soft-clip → int16
    // No pre-gain. Float has ~1500dB headroom. Soft clipper at output handles peaks.
    // This is how DAWs (Ableton, Logic, Pro Tools) handle gain staging.
    for (int ch = 0; ch < 2; ch++) {
        for (int i = ch; i < total; i += 2) {
            float x = (float)samples[i] / 32768.0f;

            // Cascade through all biquad bands
            for (int b = 0; b < s_profile.band_count; b++) {
                if (!s_profile.bands[b].enabled || s_profile.bands[b].gain_db == 0.0f)
                    continue;

                biquad_t *bq = &s_filters[b];
                float y = bq->b0 * x + bq->z1[ch];
                bq->z1[ch] = bq->b1 * x - bq->a1 * y + bq->z2[ch];
                bq->z2[ch] = bq->b2 * x - bq->a2 * y;
                x = y;
            }

            // Soft clip (polynomial knee at ±0.95, asymptotes to ±1.0)
            if (x > 0.95f) {
                float excess = x - 0.95f;
                x = 0.95f + 0.05f * tanhf(excess / 0.05f);
            } else if (x < -0.95f) {
                float excess = -x - 0.95f;
                x = -(0.95f + 0.05f * tanhf(excess / 0.05f));
            }

            samples[i] = (int16_t)(x * 32767.0f);
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

void audio_eq_preset_harman_oe(void)
{
    memset(&s_profile, 0, sizeof(s_profile));
    strncpy(s_profile.name, "Harman OE", sizeof(s_profile.name));
    s_profile.band_count = 5;

    // Harman 2019 over-ear target curve approximation (relative to flat)
    // Bass shelf: +4dB below 80Hz
    s_profile.bands[0] = (eq_band_config_t){ .freq = 80,   .gain_db = 4.0f,  .q = 0.7f, .type = EQ_FILTER_LOW_SHELF, .enabled = true };
    // Low-mid warmth
    s_profile.bands[1] = (eq_band_config_t){ .freq = 250,  .gain_db = 0.5f,  .q = 1.0f, .type = EQ_FILTER_PEAKING,   .enabled = true };
    // Presence dip
    s_profile.bands[2] = (eq_band_config_t){ .freq = 3000, .gain_db = -2.0f, .q = 1.5f, .type = EQ_FILTER_PEAKING,   .enabled = true };
    // Treble rolloff
    s_profile.bands[3] = (eq_band_config_t){ .freq = 6000, .gain_db = -4.0f, .q = 1.0f, .type = EQ_FILTER_PEAKING,   .enabled = true };
    // Air rolloff
    s_profile.bands[4] = (eq_band_config_t){ .freq = 10000, .gain_db = -6.0f, .q = 0.7f, .type = EQ_FILTER_HIGH_SHELF, .enabled = true };

    for (int i = 0; i < s_profile.band_count; i++)
        recompute_filter(i);

    ESP_LOGI(TAG, "Loaded Harman OE 2019 preset");
}

void audio_eq_preset_harman_ie(void)
{
    memset(&s_profile, 0, sizeof(s_profile));
    strncpy(s_profile.name, "Harman IE", sizeof(s_profile.name));
    s_profile.band_count = 5;

    // Harman 2019 in-ear target curve approximation
    // Stronger bass shelf for IEMs
    s_profile.bands[0] = (eq_band_config_t){ .freq = 80,   .gain_db = 6.0f,  .q = 0.7f, .type = EQ_FILTER_LOW_SHELF, .enabled = true };
    // Slight warmth
    s_profile.bands[1] = (eq_band_config_t){ .freq = 300,  .gain_db = 0.5f,  .q = 1.0f, .type = EQ_FILTER_PEAKING,   .enabled = true };
    // Presence dip
    s_profile.bands[2] = (eq_band_config_t){ .freq = 3500, .gain_db = -3.0f, .q = 1.2f, .type = EQ_FILTER_PEAKING,   .enabled = true };
    // Treble rolloff
    s_profile.bands[3] = (eq_band_config_t){ .freq = 7000, .gain_db = -5.0f, .q = 1.0f, .type = EQ_FILTER_PEAKING,   .enabled = true };
    // Air rolloff
    s_profile.bands[4] = (eq_band_config_t){ .freq = 10000, .gain_db = -8.0f, .q = 0.7f, .type = EQ_FILTER_HIGH_SHELF, .enabled = true };

    for (int i = 0; i < s_profile.band_count; i++)
        recompute_filter(i);

    ESP_LOGI(TAG, "Loaded Harman IE 2019 preset");
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
// AutoEQ ParametricEQ.txt parser
// Format: "Filter N: ON PK Fc XXXX Hz Gain X.X dB Q X.XX"
//     or: "Filter N: ON LSC Fc XXXX Hz Gain X.X dB Q X.XX"
//     or: "Filter N: ON HSC Fc XXXX Hz Gain X.X dB Q X.XX"
// ---------------------------------------------------------------------------

esp_err_t audio_eq_load_autoeq(const char *filepath)
{
    FILE *f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open AutoEQ file: %s", filepath);
        return ESP_ERR_NOT_FOUND;
    }

    eq_profile_t prof;
    memset(&prof, 0, sizeof(prof));

    // Extract name from filename (last path component, without extension)
    const char *name = filepath;
    const char *slash = strrchr(filepath, '/');
    if (slash) name = slash + 1;
    strncpy(prof.name, name, sizeof(prof.name) - 1);
    char *dot = strrchr(prof.name, '.');
    if (dot) *dot = '\0';

    char line[256];
    while (fgets(line, sizeof(line), f) && prof.band_count < EQ_MAX_BANDS) {
        int filter_num;
        char on_off[8], type[8];
        float freq, gain, q;

        // Try parsing AutoEQ format
        if (sscanf(line, "Filter %d: %7s %7s Fc %f Hz Gain %f dB Q %f",
                   &filter_num, on_off, type, &freq, &gain, &q) == 6) {

            if (strcmp(on_off, "ON") != 0) continue;

            int idx = prof.band_count;
            prof.bands[idx].freq = freq;
            prof.bands[idx].gain_db = gain;
            prof.bands[idx].q = q;
            prof.bands[idx].enabled = true;

            if (strcmp(type, "LSC") == 0 || strcmp(type, "LS") == 0)
                prof.bands[idx].type = EQ_FILTER_LOW_SHELF;
            else if (strcmp(type, "HSC") == 0 || strcmp(type, "HS") == 0)
                prof.bands[idx].type = EQ_FILTER_HIGH_SHELF;
            else
                prof.bands[idx].type = EQ_FILTER_PEAKING;

            prof.band_count++;
            ESP_LOGI(TAG, "AutoEQ band %d: %s %.0fHz %+.1fdB Q%.2f",
                     idx, type, freq, gain, q);
        }
    }

    fclose(f);

    if (prof.band_count == 0) {
        ESP_LOGW(TAG, "No bands parsed from AutoEQ file");
        return ESP_ERR_NOT_FOUND;
    }

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

    // Reference: ISO 226 threshold of hearing at each test frequency (dB SPL).
    // 7 frequencies: 250, 500, 1k, 2k, 4k, 6k, 8k Hz (12kHz dropped).
    static const float ref_thresholds[] = {
        10.0f, 4.0f, 2.0f, -1.0f, -4.0f, 2.0f, 10.0f
    };

    out->band_count = (n > EQ_MAX_BANDS) ? EQ_MAX_BANDS : n;
    if (out->band_count > 7) out->band_count = 7;

    // Find anchor: user's best frequency (lowest threshold relative to reference)
    float best_threshold = thresholds[0];
    float best_ref = ref_thresholds[0];
    for (int i = 1; i < out->band_count; i++) {
        if (thresholds[i] - ref_thresholds[i] < best_threshold - best_ref) {
            best_threshold = thresholds[i];
            best_ref = ref_thresholds[i];
        }
    }
    float anchor = best_threshold - best_ref;

    for (int i = 0; i < out->band_count; i++) {
        float deviation = (thresholds[i] - ref_thresholds[i]) - anchor;

        // 55% partial correction (half-gain rule, matches Sonarworks/NAL-NL2)
        float gain = deviation * 0.55f;
        if (gain < -12.0f) gain = -12.0f;
        if (gain >  12.0f) gain =  12.0f;

        // Q values: wider for widely-spaced bands, narrower for close ones
        // 250→500: 1 oct, 500→1k: 1 oct, 1k→2k: 1 oct, 2k→4k: 1 oct,
        // 4k→6k: 0.6 oct, 6k→8k: 0.4 oct
        static const float q_map[] = { 0.6f, 0.7f, 0.8f, 0.9f, 1.1f, 1.1f, 0.8f };
        float q = (i < 8) ? q_map[i] : 0.7f;

        out->bands[i].freq    = test_freqs[i];
        out->bands[i].gain_db = gain;
        out->bands[i].q       = q;
        out->bands[i].type    = EQ_FILTER_PEAKING;
        out->bands[i].enabled = (fabsf(gain) > 0.5f);  // Skip negligible corrections

        ESP_LOGI(TAG, "Hearing band %d: %.0f Hz, threshold=%.1f dB, ref=%.1f, correction=%+.1f dB",
                 i, test_freqs[i], thresholds[i], ref_thresholds[i], gain);
    }
}
