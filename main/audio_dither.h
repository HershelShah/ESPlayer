#pragma once

#include <stdint.h>

// TPDF (Triangular Probability Density Function) dither.
// Converts float [-1.0, 1.0] to int16 with 1-LSB triangular dither noise.
// Eliminates idle tones and correlated quantization distortion.
//
// state: persistent LFSR state, one per channel. Init to any nonzero value.
int16_t dither_f32_to_i16(float x, uint32_t *state);

// Init dither state (call once per channel).
void dither_init(uint32_t *state);
