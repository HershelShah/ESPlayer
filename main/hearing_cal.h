#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "audio_eq.h"

// Test frequencies for hearing calibration
#define CAL_NUM_FREQS  8
extern const float CAL_TEST_FREQS[CAL_NUM_FREQS];

typedef enum {
    CAL_STATE_IDLE,       // Not running
    CAL_STATE_PLAYING,    // Playing a tone, waiting for user tap
    CAL_STATE_PAUSE,      // Brief pause between tones
    CAL_STATE_DONE,       // All frequencies tested
} cal_state_t;

typedef struct {
    cal_state_t state;
    int         current_freq_idx;          // Which frequency we're testing (0..5)
    float       current_volume;            // Current tone volume (0.0 to 1.0)
    float       thresholds[CAL_NUM_FREQS]; // Recorded thresholds (relative dB)
    bool        freq_done[CAL_NUM_FREQS];  // Which frequencies are done
} cal_status_t;

// Initialise calibration system.
void hearing_cal_init(void);

// Start a new calibration session. Resets all thresholds.
void hearing_cal_start(void);

// Called by UI when user taps "I hear it". Records threshold for current frequency.
void hearing_cal_confirm(void);

// Called every ~50ms to advance the tone ramp. Returns PCM data if calibration
// is active, otherwise returns false and the normal audio pipeline runs.
// Writes stereo 16-bit interleaved PCM into buf (frame_count frames).
bool hearing_cal_generate(int16_t *buf, int frame_count, int sample_rate);

// Get current calibration status (for UI).
const cal_status_t *hearing_cal_get_status(void);

// After calibration completes, build and apply the correction EQ profile.
void hearing_cal_apply(void);

// Save calibration results to SD card.
void hearing_cal_save(void);
