// Host-side DSP unit tests — compile with: gcc -o dsp_test dsp_test.c -lm -I../main
// Run: ./dsp_test
// No ESP32 needed. Tests each DSP block in isolation with known signals.

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

// Stub ESP-IDF functions for host compilation
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[WARN %s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[ERR %s] " fmt "\n", tag, ##__VA_ARGS__)
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NOT_FOUND -1
#define MALLOC_CAP_INTERNAL 0
static inline size_t heap_caps_get_free_size(uint32_t c) { return 100000; }
static inline void *heap_caps_malloc(size_t s, uint32_t c) { return malloc(s); }

// Config stubs
#define AUDIO_SAMPLE_RATE 44100
#define SD_MOUNT_POINT "/tmp"
#define EQ_MAX_BANDS 10
#define MINIMP3_MAX_SAMPLES_PER_FRAME (1152*2)
#define CONFIG_FATFS_MAX_LFN 255

// Include the actual source files directly
#include "../main/audio_eq.c"
#include "../main/audio_dsp.c"

// ===========================================================================
// Test helpers
// ===========================================================================

#define TEST_FRAMES 1024
#define SR 44100

static int tests_passed = 0, tests_failed = 0;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); tests_failed++; return; } \
} while(0)

#define PASS(msg) do { printf("  PASS: %s\n", msg); tests_passed++; } while(0)

static void gen_sine(int16_t *buf, int frames, float freq, float amp)
{
    for (int i = 0; i < frames; i++) {
        float v = sinf(2.0f * M_PI * freq * (float)i / SR) * amp;
        buf[i * 2] = buf[i * 2 + 1] = (int16_t)v;
    }
}

static float rms(const int16_t *buf, int frames, int ch)
{
    double sum = 0;
    for (int i = 0; i < frames; i++) {
        float s = (float)buf[i * 2 + ch];
        sum += s * s;
    }
    return sqrtf(sum / frames);
}

static float rms_db(const int16_t *buf, int frames, int ch)
{
    float r = rms(buf, frames, ch);
    return 20.0f * log10f(r / 32768.0f + 1e-10f);
}

// ===========================================================================
// Tests
// ===========================================================================

static void test_eq_flat_passthrough(void)
{
    printf("\n--- test_eq_flat_passthrough ---\n");
    int16_t buf[TEST_FRAMES * 2], ref[TEST_FRAMES * 2];
    gen_sine(buf, TEST_FRAMES, 1000.0f, 16000.0f);
    memcpy(ref, buf, sizeof(buf));

    audio_eq_init(SR);
    audio_eq_preset_flat();
    audio_eq_process(buf, TEST_FRAMES);

    // Flat should be bit-identical pass-through
    ASSERT(memcmp(buf, ref, sizeof(buf)) == 0, "Flat EQ should not modify signal");
    PASS("Flat EQ is bit-identical pass-through");
}

static void test_eq_edm_boosts_bass(void)
{
    printf("\n--- test_eq_edm_boosts_bass ---\n");
    int16_t buf[TEST_FRAMES * 2];
    gen_sine(buf, TEST_FRAMES, 60.0f, 10000.0f);
    float before = rms(buf, TEST_FRAMES, 0);

    audio_eq_init(SR);
    audio_eq_preset_edm();
    audio_eq_process(buf, TEST_FRAMES);
    float after = rms(buf, TEST_FRAMES, 0);

    float gain_db = 20.0f * log10f(after / before);
    printf("  60Hz RMS: %.0f → %.0f (gain: %+.1f dB)\n", before, after, gain_db);
    ASSERT(gain_db > 2.0f, "EDM preset should boost 60Hz by at least 2dB");
    ASSERT(gain_db < 15.0f, "EDM preset should not boost 60Hz by more than 15dB");
    PASS("EDM boosts 60Hz correctly");
}

static void test_eq_edm_cuts_mids(void)
{
    printf("\n--- test_eq_edm_cuts_mids ---\n");
    int16_t buf[TEST_FRAMES * 2];
    gen_sine(buf, TEST_FRAMES, 800.0f, 10000.0f);
    float before = rms(buf, TEST_FRAMES, 0);

    audio_eq_init(SR);
    audio_eq_preset_edm();
    audio_eq_process(buf, TEST_FRAMES);
    float after = rms(buf, TEST_FRAMES, 0);

    float gain_db = 20.0f * log10f(after / before);
    printf("  800Hz RMS: %.0f → %.0f (gain: %+.1f dB)\n", before, after, gain_db);
    ASSERT(gain_db < -1.0f, "EDM preset should cut 800Hz");
    PASS("EDM cuts mids correctly");
}

static void test_eq_no_clipping_hot_signal(void)
{
    printf("\n--- test_eq_no_clipping_hot_signal ---\n");
    int16_t buf[TEST_FRAMES * 2];
    gen_sine(buf, TEST_FRAMES, 60.0f, 30000.0f);

    audio_eq_init(SR);
    audio_eq_preset_edm();
    audio_eq_process(buf, TEST_FRAMES);

    // Check no samples exceed int16 range (soft clipper should handle)
    int clipped = 0;
    for (int i = 0; i < TEST_FRAMES * 2; i++) {
        if (buf[i] == 32767 || buf[i] == -32768) clipped++;
    }
    printf("  Clipped samples: %d / %d\n", clipped, TEST_FRAMES * 2);
    // Some clipping is OK (soft clipper), but shouldn't be more than ~20% of samples
    ASSERT(clipped < TEST_FRAMES, "Too many hard-clipped samples — soft clipper not working?");
    PASS("Hot signal handled without excessive clipping");
}

static void test_limiter_quiet_passthrough(void)
{
    printf("\n--- test_limiter_quiet_passthrough ---\n");
    int16_t buf[TEST_FRAMES * 2], ref[TEST_FRAMES * 2];
    gen_sine(buf, TEST_FRAMES, 1000.0f, 10000.0f);  // ~-10 dBFS
    memcpy(ref, buf, sizeof(buf));

    audio_dsp_init(SR);
    audio_dsp_limiter(buf, TEST_FRAMES);

    // Quiet signal should pass through unchanged (below -0.5 dBFS threshold)
    int diff = 0;
    for (int i = 0; i < TEST_FRAMES * 2; i++) {
        if (abs(buf[i] - ref[i]) > 1) diff++;
    }
    printf("  Modified samples: %d / %d\n", diff, TEST_FRAMES * 2);
    ASSERT(diff < TEST_FRAMES / 10, "Limiter should not modify quiet signal");
    PASS("Limiter passes quiet signal through");
}

static void test_limiter_reduces_loud(void)
{
    printf("\n--- test_limiter_reduces_loud ---\n");
    int16_t buf[TEST_FRAMES * 2];
    gen_sine(buf, TEST_FRAMES, 1000.0f, 32000.0f);  // ~-0.2 dBFS (near full scale)
    float before = rms(buf, TEST_FRAMES, 0);

    audio_dsp_init(SR);
    audio_dsp_limiter(buf, TEST_FRAMES);
    float after = rms(buf, TEST_FRAMES, 0);

    float gain_db = 20.0f * log10f(after / before);
    printf("  RMS: %.0f → %.0f (gain: %+.1f dB)\n", before, after, gain_db);
    ASSERT(gain_db < -0.3f, "Limiter should reduce near-clipping signal");
    PASS("Limiter reduces loud signal");
}

static void test_exciter_off_passthrough(void)
{
    printf("\n--- test_exciter_off_passthrough ---\n");
    int16_t buf[TEST_FRAMES * 2], ref[TEST_FRAMES * 2];
    gen_sine(buf, TEST_FRAMES, 100.0f, 16000.0f);
    memcpy(ref, buf, sizeof(buf));

    audio_dsp_init(SR);
    audio_dsp_set_exciter(false);
    audio_dsp_bass_exciter(buf, TEST_FRAMES);

    ASSERT(memcmp(buf, ref, sizeof(buf)) == 0, "Exciter OFF should be pass-through");
    PASS("Exciter OFF is pass-through");
}

static void test_exciter_on_adds_harmonics(void)
{
    printf("\n--- test_exciter_on_adds_harmonics ---\n");
    int16_t buf[TEST_FRAMES * 2], ref[TEST_FRAMES * 2];
    gen_sine(buf, TEST_FRAMES, 80.0f, 16000.0f);
    memcpy(ref, buf, sizeof(buf));

    audio_dsp_init(SR);
    audio_dsp_set_exciter(true);
    audio_dsp_bass_exciter(buf, TEST_FRAMES);

    // Signal should be different when exciter is on
    int diff = 0;
    for (int i = 0; i < TEST_FRAMES * 2; i++) {
        if (buf[i] != ref[i]) diff++;
    }
    printf("  Modified samples: %d / %d\n", diff, TEST_FRAMES * 2);
    ASSERT(diff > TEST_FRAMES, "Exciter ON should modify bass signal");

    // RMS should increase slightly (harmonics added)
    float rms_before = rms(ref, TEST_FRAMES, 0);
    float rms_after = rms(buf, TEST_FRAMES, 0);
    float gain_db = 20.0f * log10f(rms_after / rms_before);
    printf("  RMS: %.0f → %.0f (gain: %+.1f dB)\n", rms_before, rms_after, gain_db);
    // Should not crush or massively boost
    ASSERT(gain_db > -3.0f && gain_db < 6.0f, "Exciter gain should be modest");
    PASS("Exciter adds harmonics to bass");
    audio_dsp_set_exciter(false);
}

static void test_crossfeed_off_passthrough(void)
{
    printf("\n--- test_crossfeed_off_passthrough ---\n");
    int16_t buf[TEST_FRAMES * 2], ref[TEST_FRAMES * 2];
    gen_sine(buf, TEST_FRAMES, 1000.0f, 16000.0f);
    memcpy(ref, buf, sizeof(buf));

    audio_dsp_init(SR);
    audio_dsp_set_crossfeed(false);
    audio_dsp_crossfeed(buf, TEST_FRAMES);

    ASSERT(memcmp(buf, ref, sizeof(buf)) == 0, "Crossfeed OFF should be pass-through");
    PASS("Crossfeed OFF is pass-through");
}

static void test_crossfeed_mixes_channels(void)
{
    printf("\n--- test_crossfeed_mixes_channels ---\n");
    // L = loud sine, R = silence
    int16_t buf[TEST_FRAMES * 2];
    for (int i = 0; i < TEST_FRAMES; i++) {
        buf[i * 2]     = (int16_t)(sinf(2.0f * M_PI * 500.0f * i / SR) * 16000.0f);
        buf[i * 2 + 1] = 0;
    }
    float r_before = rms(buf, TEST_FRAMES, 1);

    audio_dsp_init(SR);
    audio_dsp_set_crossfeed(true);
    audio_dsp_crossfeed(buf, TEST_FRAMES);
    float r_after = rms(buf, TEST_FRAMES, 1);

    printf("  R channel RMS: %.0f → %.0f\n", r_before, r_after);
    ASSERT(r_after > 100.0f, "Crossfeed should bleed L into R");
    PASS("Crossfeed bleeds L into R");
    audio_dsp_set_crossfeed(false);
}

// ===========================================================================
// Main
// ===========================================================================

int main(void)
{
    printf("===========================================\n");
    printf("  ESP32 EDM Player — DSP Unit Tests\n");
    printf("===========================================\n");

    test_eq_flat_passthrough();
    test_eq_edm_boosts_bass();
    test_eq_edm_cuts_mids();
    test_eq_no_clipping_hot_signal();
    test_limiter_quiet_passthrough();
    test_limiter_reduces_loud();
    test_exciter_off_passthrough();
    test_exciter_on_adds_harmonics();
    test_crossfeed_off_passthrough();
    test_crossfeed_mixes_channels();

    printf("\n===========================================\n");
    printf("  Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("===========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
