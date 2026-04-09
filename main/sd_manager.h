#pragma once

#include "esp_err.h"
#include "config.h"

// Track entry — just the filename (no path prefix, no metadata yet)
typedef struct {
    char filename[CONFIG_FATFS_MAX_LFN + 1];
} track_entry_t;

// Track list populated by sd_manager_scan()
typedef struct {
    track_entry_t tracks[MAX_TRACKS];
    int           count;
} track_list_t;

// Mount the SD card on SPI bus. Returns ESP_OK on success.
esp_err_t sd_manager_init(void);

// Scan MUSIC_DIR for .mp3 files. Fills out `list`. Returns ESP_OK on success.
esp_err_t sd_manager_scan(track_list_t *list);

// Unmount the SD card.
void sd_manager_deinit(void);
