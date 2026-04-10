#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    AUDIO_CMD_PLAY,       // payload = track index
    AUDIO_CMD_STOP,
    AUDIO_CMD_PAUSE,
    AUDIO_CMD_RESUME,
    AUDIO_CMD_NEXT,
    AUDIO_CMD_PREV,
} audio_cmd_type_t;

typedef struct {
    audio_cmd_type_t cmd;
    int              arg;   // e.g. track index for PLAY
} audio_cmd_t;

typedef enum {
    AUDIO_STATE_IDLE,
    AUDIO_STATE_PLAYING,
    AUDIO_STATE_PAUSED,
} audio_state_t;

// Initialise the audio pipeline: creates ring buffer, command queue,
// and spawns the decode task on Core 1. Call AFTER sd_manager_init().
esp_err_t audio_pipeline_init(void);

// Send a command to the audio task.
esp_err_t audio_pipeline_cmd(audio_cmd_type_t cmd, int arg);

// Read PCM data from the ring buffer. Called by BT A2DP data callback.
// Returns number of bytes read. Fills with silence if not enough data.
int audio_pipeline_read_pcm(uint8_t *buf, int len);

// Get current state.
audio_state_t audio_pipeline_get_state(void);

// Get the index of the currently playing track (-1 if idle).
int audio_pipeline_get_current_track(void);

// Set volume (0-255).
void audio_pipeline_set_volume(uint8_t vol);
uint8_t audio_pipeline_get_volume(void);

// Get underrun stats: how many times the BT callback got insufficient data.
void audio_pipeline_get_stats(uint32_t *underruns, uint32_t *total);

// Start dumping processed PCM to serial (10 seconds).
// Output format: "PCM:LLLLRRRR\n" (hex int16 L+R per line), ends with "PCM:END\n".
void audio_pipeline_start_pcm_dump(void);

// Generate a test tone, process through DSP chain, dump to serial.
// No BT/SD/music needed — for measurement only.
void audio_pipeline_test_dump(void);
