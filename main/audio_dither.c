#include "audio_dither.h"

// Fast 32-bit LFSR PRNG (Galois form, period 2^32-1)
static inline uint32_t lfsr_next(uint32_t *state)
{
    uint32_t s = *state;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    *state = s;
    return s;
}

void dither_init(uint32_t *state)
{
    *state = 0xDEADBEEF;  // Any nonzero seed
}

int16_t dither_f32_to_i16(float x, uint32_t *state)
{
    // Scale float [-1.0, 1.0] to int16 range
    float scaled = x * 32768.0f;

    // TPDF dither: sum of two uniform random values in [-1, +1]
    // This gives triangular distribution in [-2, +2] with 1 LSB RMS
    uint32_t r1 = lfsr_next(state);
    uint32_t r2 = lfsr_next(state);
    // Convert uint32 to float in [-1, 1]
    float d1 = (float)(int32_t)r1 / 2147483648.0f;
    float d2 = (float)(int32_t)r2 / 2147483648.0f;
    float dither = d1 + d2;  // Triangular [-2, +2]

    // Add dither and round
    float result = scaled + dither;

    // Clamp to int16 range
    if (result > 32767.0f)  result = 32767.0f;
    if (result < -32768.0f) result = -32768.0f;

    return (int16_t)result;
}
