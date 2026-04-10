#include "audio_dither.h"
#include <math.h>

// Fast 32-bit xorshift PRNG (period 2^32-1)
static inline uint32_t lfsr_next(uint32_t *state)
{
    uint32_t s = *state;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    *state = s;
    return s;
}

// Convert uint32 to float in [-1.0, 1.0]
static inline float rand_float(uint32_t *state)
{
    return (float)(int32_t)lfsr_next(state) / 2147483648.0f;
}

void dither_init(dither_state_t *st)
{
    st->lfsr = 0xDEADBEEF;
    st->error[0] = 0.0f;
    st->error[1] = 0.0f;
}

int16_t dither_tpdf(float x, dither_state_t *st)
{
    float scaled = x * 32768.0f;

    // TPDF: sum of two uniform → triangular [-2, +2]
    float dither = rand_float(&st->lfsr) + rand_float(&st->lfsr);

    float result = scaled + dither;
    if (result > 32767.0f)  result = 32767.0f;
    if (result < -32768.0f) result = -32768.0f;
    return (int16_t)result;
}

int16_t dither_noise_shaped(float x, dither_state_t *st)
{
    float scaled = x * 32768.0f;

    // TPDF dither
    float dither = rand_float(&st->lfsr) + rand_float(&st->lfsr);

    // 1st-order noise shaping: NTF(z) = 1 - z^-1 (highpass)
    // SUBTRACT previous error to push noise to high frequencies.
    // y[n] = x[n] - e[n-1] + dither
    // q[n] = round(y[n])
    // e[n] = q[n] - y[n]  (error of shaped signal, stays bounded)
    float shaped = scaled - st->error[0] + dither;

    // Round to nearest integer
    float quantized;
    if (shaped >= 0)
        quantized = (float)(int32_t)(shaped + 0.5f);
    else
        quantized = (float)(int32_t)(shaped - 0.5f);

    // Clamp
    if (quantized > 32767.0f)  quantized = 32767.0f;
    if (quantized < -32768.0f) quantized = -32768.0f;

    // Error = quantized - shaped (NOT quantized - scaled)
    st->error[0] = quantized - shaped;

    return (int16_t)quantized;
}
