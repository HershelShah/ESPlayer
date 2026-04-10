#include "audio_dsp.h"
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------
static int   s_sr;
static bool  s_exciter_on  = false;
static bool  s_crossfeed_on = false;

// audio_dsp_init() is at the end of this file (needs all statics declared first)

// ===========================================================================
// 1. Peak Limiter — envelope follower
// ===========================================================================

static float s_lim_env = 0.0f;

void audio_dsp_limiter(int16_t *samples, int frame_count)
{
    // Simple peak limiter: instant attack, smooth release.
    // If any sample exceeds threshold, reduce gain immediately.
    // Release: gain recovers at ~50ms time constant.
    const float threshold = 0.90f;   // -0.9 dBFS
    const float release   = 0.99977f; // ~100ms @ 44100Hz
    float env = s_lim_env;

    for (int i = 0; i < frame_count * 2; i += 2) {
        float l = (float)samples[i]     / 32768.0f;
        float r = (float)samples[i + 1] / 32768.0f;
        float peak = fabsf(l);
        float rp   = fabsf(r);
        if (rp > peak) peak = rp;

        // Instant attack: envelope jumps to peak immediately
        if (peak > env)
            env = peak;
        else
            env = peak + release * (env - peak);

        // Gain reduction
        if (env > threshold) {
            float gain = threshold / env;
            samples[i]     = (int16_t)((float)samples[i]     * gain);
            samples[i + 1] = (int16_t)((float)samples[i + 1] * gain);
        }
    }
    s_lim_env = env;
}

// ===========================================================================
// 2. Volume-Dependent Loudness Compensation (Fletcher-Munson / ISO 226)
// ===========================================================================
//
// At low volume, boost bass and treble to compensate for ear insensitivity.
// Uses two biquad shelving filters whose gains vary with volume.

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1[2], z2[2];
} loud_biquad_t;

static loud_biquad_t s_loud_lo, s_loud_hi;
static uint8_t       s_loud_last_vol = 255;

static void loud_compute_low_shelf(loud_biquad_t *bq, float fs, float f0, float gain_db, float Q)
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

static void loud_compute_high_shelf(loud_biquad_t *bq, float fs, float f0, float gain_db, float Q)
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

static void loud_apply_biquad(loud_biquad_t *bq, int16_t *samples, int frame_count)
{
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

// ISO 226:2003 compensation at key frequencies.
// Difference in SPL between 80 phon (reference) and 40 phon (quiet listening).
// Positive = needs boost at quiet levels. Normalized to 0 at 1kHz.
// ISO 226:2003 compensation at key frequencies (40 phon vs 80 phon).
// Positive = needs boost at quiet levels. Normalized to 0 at 1kHz.
static const float ISO226_COMP_40[]    = { 28.7, 13.4, 0.0, -3.3, 11.8 };
// Frequencies: 63, 200, 1000, 4000, 8000 Hz
// These represent: at 40 phon, you need +28.7dB more at 63Hz vs 1kHz to hear equally loud.

void audio_dsp_loudness(int16_t *samples, int frame_count, uint8_t volume)
{
    if (volume != s_loud_last_vol) {
        s_loud_last_vol = volume;

        // Map volume 0-255 to estimated phon level (40-80 phon range)
        // vol=255 → 80 phon (reference, no compensation)
        // vol=64  → 40 phon (full compensation)
        float vol_ratio = (float)volume / 255.0f;
        if (vol_ratio < 0.05f) vol_ratio = 0.05f;

        // Interpolation factor: 0 = 80 phon (no comp), 1 = 40 phon (full comp)
        float t = 1.0f - vol_ratio;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;

        // Compute ISO 226-derived bass and treble compensation
        // Low shelf at 80Hz: ISO 226 says +28.7dB at 63Hz for 40 phon
        float bass_boost = t * ISO226_COMP_40[0] * 0.4f;  // Scale down — full 28dB is too much
        float treble_boost = t * ISO226_COMP_40[4] * 0.3f;

        float fs = (float)s_sr;
        loud_compute_low_shelf(&s_loud_lo, fs, 80.0f, bass_boost, 0.7f);
        loud_compute_high_shelf(&s_loud_hi, fs, 8000.0f, treble_boost, 0.7f);
    }

    float vol_ratio = (float)volume / 255.0f;
    if (vol_ratio > 0.9f) return;

    loud_apply_biquad(&s_loud_lo, samples, frame_count);
    loud_apply_biquad(&s_loud_hi, samples, frame_count);
}

// ===========================================================================
// 3. Bass Harmonic Exciter
// ===========================================================================
//
// Isolate sub-bass (<80Hz), soft-clip to generate harmonics, highpass to
// remove fundamental, mix back at -10dB.

static float bass_state[2] = {0};  // 1-pole LP state for exciter

// Soft clipper: cubic approximation
static float soft_clip(float x)
{
    if (x > 1.0f)  return 1.0f;
    if (x < -1.0f) return -1.0f;
    return x * (1.5f - 0.5f * x * x);  // Cubic soft clip
}

void audio_dsp_bass_exciter(int16_t *samples, int frame_count)
{
    if (!s_exciter_on) return;

    // Simple approach: lowpass to isolate bass, soft-clip to add warmth/harmonics,
    // blend with original. No HP needed — the clipping naturally adds upper harmonics.
    const float mix = 0.3f;  // Blend amount of distorted bass

    for (int i = 0; i < frame_count * 2; i++) {
        int ch = i & 1;
        float x = (float)samples[i] / 32768.0f;

        // Lowpass to get bass content (simple 1-pole at ~200Hz for warmth)
        // alpha = 2*pi*200/44100 / (1 + 2*pi*200/44100) ≈ 0.028
        bass_state[ch] += 0.028f * (x - bass_state[ch]);
        float bass = bass_state[ch];

        // Soft-clip the bass to generate harmonics (2nd, 3rd order)
        float driven = soft_clip(bass * 3.0f);

        // Mix: original + warm bass harmonics
        float out = x + (driven - bass) * mix;  // Add only the harmonic content

        out *= 32768.0f;
        if (out > 32767.0f)  out = 32767.0f;
        if (out < -32768.0f) out = -32768.0f;
        samples[i] = (int16_t)out;
    }
}

void audio_dsp_set_exciter(bool enabled)  { s_exciter_on = enabled; }
bool audio_dsp_get_exciter(void)          { return s_exciter_on; }

// ===========================================================================
// 4. Crossfeed / BS2B
// ===========================================================================
//
// 1-pole lowpass at 700Hz, cross-mix at -4.5dB (0.596 linear).

static float s_xf_state_l = 0.0f;  // LP state for L→R
static float s_xf_state_r = 0.0f;  // LP state for R→L

void audio_dsp_crossfeed(int16_t *samples, int frame_count)
{
    if (!s_crossfeed_on) return;

    // 1-pole LP coefficient at 700Hz for 44100Hz: alpha = 2*pi*700/44100 / (1 + 2*pi*700/44100)
    const float alpha = 0.0906f;  // ~700Hz @ 44100Hz
    const float level = 0.75f;    // -2.5dB — stronger for testing

    float sl = s_xf_state_l;
    float sr = s_xf_state_r;

    for (int i = 0; i < frame_count * 2; i += 2) {
        float l = (float)samples[i];
        float r = (float)samples[i + 1];

        // Lowpass the opposite channel
        sl = sl + alpha * (r - sl);
        sr = sr + alpha * (l - sr);

        // Mix
        float out_l = l + sl * level;
        float out_r = r + sr * level;

        if (out_l > 32767.0f)  out_l = 32767.0f;
        if (out_l < -32768.0f) out_l = -32768.0f;
        if (out_r > 32767.0f)  out_r = 32767.0f;
        if (out_r < -32768.0f) out_r = -32768.0f;

        samples[i]     = (int16_t)out_l;
        samples[i + 1] = (int16_t)out_r;
    }

    s_xf_state_l = sl;
    s_xf_state_r = sr;
}

void audio_dsp_set_crossfeed(bool enabled)  { s_crossfeed_on = enabled; }
bool audio_dsp_get_crossfeed(void)          { return s_crossfeed_on; }

// ===========================================================================
// Init — must be at end of file so all statics are declared
// ===========================================================================
void audio_dsp_init(int sample_rate)
{
    s_sr = sample_rate;
    s_lim_env = 0.0f;
    memset(&s_loud_lo, 0, sizeof(s_loud_lo));
    memset(&s_loud_hi, 0, sizeof(s_loud_hi));
    s_loud_lo.b0 = 1.0f;
    s_loud_hi.b0 = 1.0f;
    s_loud_last_vol = 255;
    bass_state[0] = bass_state[1] = 0.0f;
    s_xf_state_l = 0.0f;
    s_xf_state_r = 0.0f;
}
