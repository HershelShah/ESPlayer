#pragma once

#include <stdint.h>

// TPDF (Triangular Probability Density Function) dither.
// Eliminates idle tones from float → int16 quantization.

typedef struct {
    uint32_t lfsr;  // PRNG state
} dither_state_t;

// Init dither state (call once per channel).
void dither_init(dither_state_t *st);

// TPDF dither: converts float [-1.0, 1.0] to int16 with triangular noise.
int16_t dither_tpdf(float x, dither_state_t *st);
