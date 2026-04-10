#include "audio_pipeline.h"
#include "audio_eq.h"
#include "audio_dsp.h"
#include "audio_dither.h"
#include "hearing_cal.h"
#include "config.h"
#include "sd_manager.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"

#define MINIMP3_NO_SIMD
#include "minimp3.h"

static const char *TAG = "audio";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static RingbufHandle_t  s_ringbuf      = NULL;
static QueueHandle_t    s_cmd_queue    = NULL;
static audio_state_t    s_state        = AUDIO_STATE_IDLE;
static int              s_current_track = -1;
static uint8_t          s_volume       = 200;  // 0-255, default ~78%
static uint32_t         s_underruns    = 0;     // BT callback got no data
static dither_state_t   s_dither_l;              // Noise-shaped dither state L
static dither_state_t   s_dither_r;              // Noise-shaped dither state R
static uint32_t         s_cb_total     = 0;     // Total BT callback invocations

// Serial PCM dump — captures processed audio for analysis on host.
// Set s_dump_remaining > 0 to start dumping. Frames are sent as
// base64-encoded binary lines prefixed with "PCM:" for easy parsing.
static int              s_dump_remaining = 0;
#define DUMP_FRAMES     (44100 * 10)  // 10 seconds at 44.1kHz

// ---------------------------------------------------------------------------
// Volume scaling — fixed-point multiply each sample
// ---------------------------------------------------------------------------
static void apply_volume_dithered(int16_t *samples, int count)
{
    // Apply volume in float, then dither back to int16.
    // This is the single final quantization point in the pipeline.
    float vol_scale = (float)s_volume / 255.0f;
    for (int i = 0; i < count; i += 2) {
        float l = (float)samples[i]     * vol_scale / 32768.0f;
        float r = (float)samples[i + 1] * vol_scale / 32768.0f;
        samples[i]     = dither_tpdf(l, &s_dither_l);
        samples[i + 1] = dither_tpdf(r, &s_dither_r);
    }
}

// ---------------------------------------------------------------------------
// Decode task — runs on Core 1
// ---------------------------------------------------------------------------

// Shared track list pointer (set during init, read-only after)
static const track_list_t *s_tracks = NULL;

static void decode_task(void *arg)
{
    // Allocate working buffers
    static uint8_t read_buf[SD_READ_BUF_SIZE];
    static mp3dec_t decoder;
    static mp3dec_frame_info_t frame_info;
    static int16_t pcm_buf[MINIMP3_MAX_SAMPLES_PER_FRAME];  // 2304 samples max

    mp3dec_init(&decoder);

    ESP_LOGI(TAG, "Decode task started on core %d", xPortGetCoreID());

    while (1) {
        // Wait for a command
        audio_cmd_t cmd;
        if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        if (cmd.cmd == AUDIO_CMD_PLAY) {
            int track_idx = cmd.arg;
            if (track_idx < 0 || track_idx >= s_tracks->count) {
                ESP_LOGW(TAG, "Invalid track index %d", track_idx);
                continue;
            }

            s_current_track = track_idx;
            s_state = AUDIO_STATE_PLAYING;

            // Build full path
            static char filepath[300];
            snprintf(filepath, sizeof(filepath), "%s/%s",
                     MUSIC_DIR, s_tracks->tracks[track_idx].filename);

            ESP_LOGI(TAG, "Playing [%d]: %s", track_idx, s_tracks->tracks[track_idx].filename);

            FILE *f = fopen(filepath, "rb");
            if (!f) {
                ESP_LOGE(TAG, "Failed to open: %s", filepath);
                s_state = AUDIO_STATE_IDLE;
                s_current_track = -1;
                continue;
            }

            mp3dec_init(&decoder);

            // Streaming decode loop
            int bytes_in_buf = 0;
            bool eof = false;
            bool stop_requested = false;
            int next_track = -1;

            while (!stop_requested) {
                // Check for new commands (non-blocking)
                audio_cmd_t peek;
                if (xQueueReceive(s_cmd_queue, &peek, 0) == pdTRUE) {
                    if (peek.cmd == AUDIO_CMD_STOP) {
                        stop_requested = true;
                        break;
                    } else if (peek.cmd == AUDIO_CMD_NEXT) {
                        next_track = (s_current_track + 1) % s_tracks->count;
                        stop_requested = true;
                        break;
                    } else if (peek.cmd == AUDIO_CMD_PREV) {
                        next_track = (s_current_track - 1 + s_tracks->count) % s_tracks->count;
                        stop_requested = true;
                        break;
                    } else if (peek.cmd == AUDIO_CMD_PLAY) {
                        next_track = peek.arg;
                        stop_requested = true;
                        break;
                    } else if (peek.cmd == AUDIO_CMD_PAUSE) {
                        s_state = AUDIO_STATE_PAUSED;
                        // Wait for resume or stop
                        while (1) {
                            if (xQueueReceive(s_cmd_queue, &peek, portMAX_DELAY) == pdTRUE) {
                                if (peek.cmd == AUDIO_CMD_RESUME) {
                                    s_state = AUDIO_STATE_PLAYING;
                                    break;
                                } else if (peek.cmd == AUDIO_CMD_STOP) {
                                    stop_requested = true;
                                    break;
                                } else if (peek.cmd == AUDIO_CMD_NEXT) {
                                    next_track = (s_current_track + 1) % s_tracks->count;
                                    stop_requested = true;
                                    break;
                                } else if (peek.cmd == AUDIO_CMD_PREV) {
                                    next_track = (s_current_track - 1 + s_tracks->count) % s_tracks->count;
                                    stop_requested = true;
                                    break;
                                }
                            }
                        }
                        if (stop_requested) break;
                    }
                }

                // Fill read buffer from SD card
                if (!eof && bytes_in_buf < SD_READ_BUF_SIZE) {
                    int space = SD_READ_BUF_SIZE - bytes_in_buf;
                    size_t n = fread(read_buf + bytes_in_buf, 1, space, f);
                    if (n == 0) {
                        eof = true;
                    } else {
                        bytes_in_buf += n;
                    }
                }

                if (bytes_in_buf == 0 && eof) {
                    // Track finished — auto-advance to next
                    ESP_LOGI(TAG, "Track finished, advancing");
                    next_track = (s_current_track + 1) % s_tracks->count;
                    break;
                }

                // Decode one MP3 frame
                static uint32_t frame_count = 0;
                static int64_t decode_us_total = 0;
                int64_t t0 = esp_timer_get_time();
                int samples = mp3dec_decode_frame(&decoder, read_buf, bytes_in_buf,
                                                  pcm_buf, &frame_info);
                decode_us_total += esp_timer_get_time() - t0;
                frame_count++;
                if (frame_count % 500 == 0) {
                    ESP_LOGI(TAG, "decode stats: %lu frames, avg %.1f us/frame, underruns=%lu/%lu",
                             (unsigned long)frame_count,
                             (double)decode_us_total / frame_count,
                             (unsigned long)s_underruns, (unsigned long)s_cb_total);
                }

                // Consume bytes from read buffer
                if (frame_info.frame_bytes > 0) {
                    int consumed = frame_info.frame_bytes;
                    bytes_in_buf -= consumed;
                    if (bytes_in_buf > 0) {
                        memmove(read_buf, read_buf + consumed, bytes_in_buf);
                    }
                } else if (eof) {
                    // No more frames and EOF
                    next_track = (s_current_track + 1) % s_tracks->count;
                    break;
                } else {
                    // Need more data
                    continue;
                }

                if (samples > 0) {
                    int total_samples = samples * frame_info.channels;

                    // If mono, duplicate to stereo
                    int16_t *out_buf = pcm_buf;
                    static int16_t stereo_buf[MINIMP3_MAX_SAMPLES_PER_FRAME];
                    int out_samples = total_samples;

                    if (frame_info.channels == 1) {
                        for (int i = 0; i < samples; i++) {
                            stereo_buf[i * 2]     = pcm_buf[i];
                            stereo_buf[i * 2 + 1] = pcm_buf[i];
                        }
                        out_buf = stereo_buf;
                        out_samples = samples * 2;
                    }

                    // DSP chain: EQ → Loudness → Limiter → Crossfeed → Volume
                    // Loudness before limiter so limiter catches loudness boosts.
                    // Crossfeed after limiter (only redistributes, no level change).
                    int frames = out_samples / 2;
                    audio_eq_process(out_buf, frames);
                    audio_dsp_loudness(out_buf, frames, s_volume);
                    audio_dsp_limiter(out_buf, frames);
                    audio_dsp_crossfeed(out_buf, frames);
                    apply_volume_dithered(out_buf, out_samples);

                    // Serial PCM dump (if active)
                    if (s_dump_remaining > 0) {
                        int to_dump = out_samples / 2;  // stereo frames
                        if (to_dump > s_dump_remaining) to_dump = s_dump_remaining;
                        // Print as raw hex: 4 hex chars per int16 sample, L+R interleaved
                        for (int d = 0; d < to_dump * 2; d += 2) {
                            printf("PCM:%04X%04X\n",
                                   (uint16_t)out_buf[d], (uint16_t)out_buf[d + 1]);
                        }
                        s_dump_remaining -= to_dump;
                        if (s_dump_remaining <= 0) {
                            printf("PCM:END\n");
                            ESP_LOGI(TAG, "PCM dump complete");
                        }
                    }

                    // Write to ring buffer — block until space available
                    int pcm_bytes = out_samples * sizeof(int16_t);
                    int written = 0;
                    while (written < pcm_bytes && !stop_requested) {
                        // Try to send in chunks, with timeout to check for commands
                        int remaining = pcm_bytes - written;
                        if (xRingbufferSend(s_ringbuf, (uint8_t *)out_buf + written,
                                            remaining, pdMS_TO_TICKS(50)) == pdTRUE) {
                            written = pcm_bytes;
                        } else {
                            // Ring buffer full — check for stop commands
                            audio_cmd_t check;
                            if (xQueuePeek(s_cmd_queue, &check, 0) == pdTRUE) {
                                if (check.cmd == AUDIO_CMD_STOP ||
                                    check.cmd == AUDIO_CMD_NEXT ||
                                    check.cmd == AUDIO_CMD_PREV ||
                                    check.cmd == AUDIO_CMD_PLAY) {
                                    // Will be handled at top of loop
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            fclose(f);
            ESP_LOGI(TAG, "Playback ended for track %d", track_idx);

            // If next track was requested, send ourselves a PLAY command
            if (next_track >= 0) {
                audio_cmd_t play_cmd = { .cmd = AUDIO_CMD_PLAY, .arg = next_track };
                xQueueSend(s_cmd_queue, &play_cmd, 0);
            } else {
                s_state = AUDIO_STATE_IDLE;
                s_current_track = -1;
            }

        } else if (cmd.cmd == AUDIO_CMD_STOP) {
            s_state = AUDIO_STATE_IDLE;
            s_current_track = -1;
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t audio_pipeline_init(void)
{
    ESP_LOGI(TAG, "Initialising audio pipeline");

    // Init EQ, DSP blocks, and dither
    audio_eq_init(AUDIO_SAMPLE_RATE);
    audio_dsp_init(AUDIO_SAMPLE_RATE);
    dither_init(&s_dither_l);
    dither_init(&s_dither_r);
    s_dither_r.lfsr = 0x12345678;  // Different seed for R channel

    // Get track list (defined as global in main.c)
    extern track_list_t tracks;
    s_tracks = &tracks;

    // Ring buffer — 16KB byte buffer
    s_ringbuf = xRingbufferCreate(PCM_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!s_ringbuf) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        return ESP_ERR_NO_MEM;
    }

    // Command queue
    s_cmd_queue = xQueueCreate(8, sizeof(audio_cmd_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "Failed to create command queue");
        return ESP_ERR_NO_MEM;
    }

    // Spawn decode task on Core 1, high priority
    // minimp3 decode uses ~6-8KB stack internally for float buffers
    BaseType_t ret = xTaskCreatePinnedToCore(
        decode_task, "audio_dec", 12288, NULL, 5, NULL, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create decode task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Audio pipeline ready (ringbuf=%dB)", PCM_RINGBUF_SIZE);
    return ESP_OK;
}

esp_err_t audio_pipeline_cmd(audio_cmd_type_t cmd, int arg)
{
    audio_cmd_t c = { .cmd = cmd, .arg = arg };
    if (xQueueSend(s_cmd_queue, &c, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Command queue full");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

int audio_pipeline_read_pcm(uint8_t *buf, int len)
{
    s_cb_total++;

    // If hearing calibration is active, it takes over the audio output
    int frames = len / 4;  // stereo 16-bit = 4 bytes per frame
    if (hearing_cal_generate((int16_t *)buf, frames, AUDIO_SAMPLE_RATE)) {
        return len;
    }

    size_t item_size = 0;
    uint8_t *data = xRingbufferReceiveUpTo(s_ringbuf, &item_size, 0, len);
    if (data && item_size > 0) {
        memcpy(buf, data, item_size);
        vRingbufferReturnItem(s_ringbuf, data);
        if ((int)item_size < len) {
            memset(buf + item_size, 0, len - item_size);
            s_underruns++;
        }
        return len;
    }
    // No data — silence
    if (s_state == AUDIO_STATE_PLAYING) {
        s_underruns++;
    }
    memset(buf, 0, len);
    return len;
}

audio_state_t audio_pipeline_get_state(void)
{
    return s_state;
}

int audio_pipeline_get_current_track(void)
{
    return s_current_track;
}

void audio_pipeline_set_volume(uint8_t vol)
{
    s_volume = vol;
}

uint8_t audio_pipeline_get_volume(void)
{
    return s_volume;
}

void audio_pipeline_get_stats(uint32_t *underruns, uint32_t *total)
{
    *underruns = s_underruns;
    *total = s_cb_total;
}

void audio_pipeline_start_pcm_dump(void)
{
    s_dump_remaining = DUMP_FRAMES;
    ESP_LOGI(TAG, "Starting PCM dump (%d frames)", DUMP_FRAMES);
}

// Generate a test tone, run it through the DSP chain, dump to serial.
// No BT/SD/music needed — pure DSP measurement.
void audio_pipeline_test_dump(void)
{
    ESP_LOGI(TAG, "Generating test tone + DSP dump (997Hz, -3dBFS, 2s)");

    // Init DSP (in case not done yet)
    audio_eq_init(AUDIO_SAMPLE_RATE);
    audio_dsp_init(AUDIO_SAMPLE_RATE);
    dither_init(&s_dither_l);
    dither_init(&s_dither_r);
    s_dither_r.lfsr = 0x12345678;

    // Generate 2s of audio, process through DSP, dump at reduced rate.
    // Serial @ 115200 baud can handle ~800 lines/sec.
    // We output every 4th sample = 11025 lines/sec... still too fast.
    // Instead: process full-rate but only dump 8820 samples (0.2s worth)
    // after 0.5s warmup. This gives enough data for FFT analysis.
    int total_frames = AUDIO_SAMPLE_RATE * 1;  // 1 second total
    int warmup_frames = AUDIO_SAMPLE_RATE / 4; // 0.25s warmup (filter settle)
    int dump_frames = 8820;                     // 0.2s to dump (enough for FFT)
    int chunk = 256;
    static int16_t buf[256 * 2];
    float phase = 0.0f;
    float phase_inc = 2.0f * 3.14159265f * 997.0f / (float)AUDIO_SAMPLE_RATE;
    float amp = 32768.0f * 0.707f;  // -3 dBFS

    int frames_done = 0;
    int dumped = 0;
    printf("PCM:START\n");
    fflush(stdout);

    for (int done = 0; done < total_frames; done += chunk) {
        int n = chunk;
        if (done + n > total_frames) n = total_frames - done;

        // Generate stereo 997Hz sine
        for (int i = 0; i < n; i++) {
            int16_t val = (int16_t)(sinf(phase) * amp);
            buf[i * 2]     = val;
            buf[i * 2 + 1] = val;
            phase += phase_inc;
            if (phase >= 2.0f * 3.14159265f) phase -= 2.0f * 3.14159265f;
        }

        // Run through DSP chain
        audio_eq_process(buf, n);
        audio_dsp_loudness(buf, n, s_volume);
        audio_dsp_limiter(buf, n);
        audio_dsp_crossfeed(buf, n);
        apply_volume_dithered(buf, n * 2);

        frames_done += n;

        // Only dump after warmup, and only dump_frames samples
        if (frames_done > warmup_frames && dumped < dump_frames) {
            for (int i = 0; i < n * 2 && dumped < dump_frames; i += 2) {
                printf("PCM:%04X%04X\n", (uint16_t)buf[i], (uint16_t)buf[i + 1]);
                dumped++;
            }
            fflush(stdout);
        }
    }

    printf("PCM:END\n");
    ESP_LOGI(TAG, "Test dump complete");
}
