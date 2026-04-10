#include "audio_dsp.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------
static int   s_sr;
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
// 4. Crossfeed (Bauer-inspired, simplified BS2B)
// ===========================================================================
//
// Proper crossfeed simulates acoustic crosstalk in a room:
// 1. Lowpass the opposite channel (head shadow = HF attenuation)
// 2. Delay the crossfed signal (interaural time difference, ~7 samples @ 44.1kHz ≈ 160µs)
// 3. Attenuate the direct channel's bass (both ears hear LF equally in a room)
// 4. Mix at -4.5dB (BS2B default level)

#define XF_DELAY 7  // ~160µs ITD at 44.1kHz
static float s_xf_lp_l = 0.0f;   // LP state for L→R crossfeed
static float s_xf_lp_r = 0.0f;   // LP state for R→L crossfeed
static float s_xf_dl_l[XF_DELAY]; // Delay line L→R
static float s_xf_dl_r[XF_DELAY]; // Delay line R→L
static int   s_xf_dl_idx = 0;     // Circular buffer index
static float s_xf_bass_l = 0.0f;  // Direct channel bass LP state
static float s_xf_bass_r = 0.0f;

void audio_dsp_crossfeed(int16_t *samples, int frame_count)
{
    if (!s_crossfeed_on) return;

    const float lp_alpha = 0.0906f;   // 1-pole LP at ~700Hz (head shadow)
    const float xf_level = 0.596f;    // -4.5dB crossfeed level (BS2B default)
    const float bass_alpha = 0.028f;   // 1-pole LP at ~200Hz for bass attenuation
    const float bass_atten = 0.3f;     // Reduce direct bass by 30% (simulates equal-ear LF)

    for (int i = 0; i < frame_count * 2; i += 2) {
        float l = (float)samples[i];
        float r = (float)samples[i + 1];

        // 1. Lowpass the opposite channel (head shadow filter)
        s_xf_lp_l = s_xf_lp_l + lp_alpha * (r - s_xf_lp_l);
        s_xf_lp_r = s_xf_lp_r + lp_alpha * (l - s_xf_lp_r);

        // 2. Delay the crossfed signal (ITD)
        int idx = s_xf_dl_idx;
        float delayed_l = s_xf_dl_l[idx];  // Read old value
        float delayed_r = s_xf_dl_r[idx];
        s_xf_dl_l[idx] = s_xf_lp_l;        // Write new value
        s_xf_dl_r[idx] = s_xf_lp_r;
        s_xf_dl_idx = (idx + 1) % XF_DELAY;

        // 3. Attenuate direct channel bass (both ears hear LF equally in rooms)
        s_xf_bass_l = s_xf_bass_l + bass_alpha * (l - s_xf_bass_l);
        s_xf_bass_r = s_xf_bass_r + bass_alpha * (r - s_xf_bass_r);
        float l_direct = l - s_xf_bass_l * bass_atten;
        float r_direct = r - s_xf_bass_r * bass_atten;

        // 4. Mix: attenuated direct + delayed crossfed opposite
        float out_l = l_direct + delayed_l * xf_level;
        float out_r = r_direct + delayed_r * xf_level;

        if (out_l > 32767.0f)  out_l = 32767.0f;
        if (out_l < -32768.0f) out_l = -32768.0f;
        if (out_r > 32767.0f)  out_r = 32767.0f;
        if (out_r < -32768.0f) out_r = -32768.0f;

        samples[i]     = (int16_t)out_l;
        samples[i + 1] = (int16_t)out_r;
    }
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
    s_xf_lp_l = s_xf_lp_r = 0.0f;
    s_xf_bass_l = s_xf_bass_r = 0.0f;
    s_xf_dl_idx = 0;
    memset(s_xf_dl_l, 0, sizeof(s_xf_dl_l));
    memset(s_xf_dl_r, 0, sizeof(s_xf_dl_r));
}
