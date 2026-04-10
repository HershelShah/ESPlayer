#include "audio_dither.h"

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
