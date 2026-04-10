#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "audio_eq.h"

// Test frequencies — 7 points from 250Hz to 8kHz.
// 12kHz dropped: SBC codec rolls off 6-15dB above 10kHz, corrupting measurements.
#define CAL_NUM_FREQS  7
extern const float CAL_TEST_FREQS[CAL_NUM_FREQS];

typedef enum {
    CAL_STATE_IDLE,
    CAL_STATE_PLAYING,    // Playing a tone, waiting for response
    CAL_STATE_PAUSE,      // Brief silence between tones
    CAL_STATE_WAITING,    // Tone off, waiting to see if user taps (catch trial / descend)
    CAL_STATE_DONE,
} cal_state_t;

typedef enum {
    CAL_EAR_LEFT,
    CAL_EAR_RIGHT,
} cal_ear_t;

typedef struct {
    cal_state_t state;
    cal_ear_t   current_ear;               // Which ear we're testing
    int         current_freq_idx;          // Which frequency (0..6)
    float       current_volume_db;         // Current tone level in dB relative to start
    float       thresholds_l[CAL_NUM_FREQS]; // Left ear thresholds (dB)
    float       thresholds_r[CAL_NUM_FREQS]; // Right ear thresholds (dB)
    bool        freq_done[CAL_NUM_FREQS];

    // Adaptive staircase state
    int         reversals;                 // Count of direction changes
    int         last_direction;            // +1 = ascending, -1 = descending
    float       reversal_levels[8];        // dB levels at each reversal
    int         step_db;                   // Current step size (6 or 3 dB)
    bool        user_responded;            // Did user tap during current presentation?
} cal_status_t;

void hearing_cal_init(void);
void hearing_cal_start(void);

// User tapped "I hear it"
void hearing_cal_confirm(void);

// User tapped "I don't hear it" / tone timed out
void hearing_cal_no_response(void);

// Generate audio. Returns true if calibration is active.
bool hearing_cal_generate(int16_t *buf, int frame_count, int sample_rate);

const cal_status_t *hearing_cal_get_status(void);
void hearing_cal_apply(void);
void hearing_cal_save(void);
