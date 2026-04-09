#pragma once

#include <stdint.h>
#include <stdbool.h>

// Initialise all DSP blocks. Call once after audio_eq_init().
void audio_dsp_init(int sample_rate);

// --- Peak Limiter ---
// Envelope follower: 0.1ms attack, 50ms release, threshold -0.5 dBFS.
// Prevents clipping after EQ boosts. Always active.
void audio_dsp_limiter(int16_t *samples, int frame_count);

// --- Volume-Dependent Loudness Compensation (Fletcher-Munson) ---
// Boosts bass/treble at low volumes to compensate for human hearing curves.
// volume: 0-255 (same scale as audio_pipeline volume).
void audio_dsp_loudness(int16_t *samples, int frame_count, uint8_t volume);

// --- Bass Harmonic Exciter ---
// Generates 2nd/3rd harmonics of sub-bass content to make bass "felt"
// on headphones with poor low-frequency extension. Toggle-able.
void audio_dsp_bass_exciter(int16_t *samples, int frame_count);
void audio_dsp_set_exciter(bool enabled);
bool audio_dsp_get_exciter(void);

// --- Crossfeed / BS2B ---
// Mixes lowpassed opposite channel to reduce headphone fatigue.
// Bauer BS2B algorithm: 1-pole LP at 700Hz, -4.5dB crossfeed. Toggle-able.
void audio_dsp_crossfeed(int16_t *samples, int frame_count);
void audio_dsp_set_crossfeed(bool enabled);
bool audio_dsp_get_crossfeed(void);
