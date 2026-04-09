#include "sd_manager.h"

#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd_mgr";

static sdmmc_card_t *s_card = NULL;

esp_err_t sd_manager_init(void)
{
    ESP_LOGI(TAG, "Initialising SD card (SPI mode)");

    // Host: SD card uses SPI3_HOST (VSPI) — pins 18/19/23.
    // SPI2_HOST (HSPI) is reserved for the ILI9341 display.
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;

    // SPI bus configuration
    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = SD_MOSI,
        .miso_io_num   = SD_MISO,
        .sclk_io_num   = SD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    // Slot configuration — CS pin
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS;
    slot_config.host_id = host.slot;

    // Mount FAT filesystem
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config,
                                  &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        return ret;
    }

    // Log card info
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

// Check if filename ends with ".mp3" (case-insensitive)
static bool is_mp3(const char *name)
{
    size_t len = strlen(name);
    if (len < 5) return false;  // x.mp3 = 5 chars minimum
    const char *ext = name + len - 4;
    return (ext[0] == '.' &&
            (ext[1] == 'm' || ext[1] == 'M') &&
            (ext[2] == 'p' || ext[2] == 'P') &&
            (ext[3] == '3'));
}

esp_err_t sd_manager_scan(track_list_t *list)
{
    if (!list) return ESP_ERR_INVALID_ARG;

    list->count = 0;

    DIR *dir = opendir(MUSIC_DIR);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open %s", MUSIC_DIR);
        return ESP_ERR_NOT_FOUND;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && list->count < MAX_TRACKS) {
        // Skip directories
        if (entry->d_type == DT_DIR) continue;

        if (is_mp3(entry->d_name)) {
            strncpy(list->tracks[list->count].filename, entry->d_name,
                    sizeof(list->tracks[0].filename) - 1);
            list->tracks[list->count].filename[sizeof(list->tracks[0].filename) - 1] = '\0';
            ESP_LOGI(TAG, "  [%d] %s", list->count, list->tracks[list->count].filename);
            list->count++;
        }
    }

    closedir(dir);
    ESP_LOGI(TAG, "Found %d MP3 file(s) in %s", list->count, MUSIC_DIR);
    return ESP_OK;
}

void sd_manager_deinit(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card = NULL;
        ESP_LOGI(TAG, "SD card unmounted");
    }
}
