#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// 10-band parametric EQ with save/load to SD card
#define EQ_MAX_BANDS  10

typedef enum {
    EQ_FILTER_PEAKING,
    EQ_FILTER_LOW_SHELF,
    EQ_FILTER_HIGH_SHELF,
} eq_filter_type_t;

typedef struct {
    float             freq;       // Centre/corner frequency (Hz)
    float             gain_db;    // -12 to +12 dB
    float             q;          // Q factor (0.1 to 10.0)
    eq_filter_type_t  type;
    bool              enabled;
} eq_band_config_t;

typedef struct {
    char              name[32];   // Profile name (e.g. "EDM", "Flat", "My Hearing")
    eq_band_config_t  bands[EQ_MAX_BANDS];
    int               band_count; // How many bands are active (0 = bypass)
} eq_profile_t;

// Initialise the EQ for a given sample rate. Call once.
void audio_eq_init(int sample_rate);

// Load a profile (copies band configs and recomputes all filters).
void audio_eq_load_profile(const eq_profile_t *profile);

// Get the currently active profile (read-only).
const eq_profile_t *audio_eq_get_profile(void);

// Set a single band's gain. Recomputes that band's filter.
void audio_eq_set_band_gain(int band_index, float gain_db);

// Bypass all EQ processing (pass-through).
void audio_eq_set_bypass(bool bypass);
bool audio_eq_get_bypass(void);

// Process interleaved stereo 16-bit PCM samples in-place.
void audio_eq_process(int16_t *samples, int frame_count);

// Save current profile to SD card: /sdcard/eq_profiles/<name>.txt
esp_err_t audio_eq_save_profile(const char *filename);

// Load profile from SD card (native format).
esp_err_t audio_eq_load_profile_file(const char *filename);

// Load AutoEQ ParametricEQ.txt format (full path).
// Supports 6000+ headphone profiles from github.com/jaakkopasanen/AutoEq
esp_err_t audio_eq_load_autoeq(const char *filepath);

// --- Built-in presets ---
void audio_eq_preset_flat(void);
void audio_eq_preset_edm(void);
void audio_eq_preset_harman_oe(void);
void audio_eq_preset_harman_ie(void);

// Build a correction profile from hearing calibration thresholds.
// thresholds[]: dB SPL at which user hears each test frequency.
// test_freqs[]: the frequencies tested.
// n: number of test points.
void audio_eq_build_hearing_profile(const float *test_freqs, const float *thresholds,
                                    int n, eq_profile_t *out);
