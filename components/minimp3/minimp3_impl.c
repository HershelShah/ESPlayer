// Pull in the minimp3 implementation exactly once.
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD     // ESP32 has no SIMD — avoid x86 intrinsics
#define MINIMP3_ONLY_MP3    // We only play MP3 (Layer 3) — saves code + stack
#include "minimp3.h"
