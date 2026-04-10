#pragma once

#include <stdint.h>

// Dither modes for float → int16 conversion.

typedef struct {
    uint32_t lfsr;      // PRNG state
    float    error[2];  // Noise shaping error feedback (2nd order)
} dither_state_t;

// Init dither state (call once per channel).
void dither_init(dither_state_t *st);

// TPDF dither: flat noise spectrum, eliminates idle tones.
int16_t dither_tpdf(float x, dither_state_t *st);

// Noise-shaped dither: pushes noise above 10kHz.
// Same idle tone elimination + ~6dB perceptual improvement.
int16_t dither_noise_shaped(float x, dither_state_t *st);
