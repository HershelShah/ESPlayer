#include "bt_a2dp_source.h"
#include "audio_pipeline.h"
#include "config.h"

#include <string.h>

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bt_a2dp";

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static bt_device_t  s_devices[BT_MAX_DISCOVERED];
static int          s_device_count = 0;
static bt_state_t   s_state = BT_STATE_IDLE;

// ---------------------------------------------------------------------------
// A2DP source data callback — called by BT stack to get PCM data
// ---------------------------------------------------------------------------
static int32_t bt_a2dp_data_cb(uint8_t *buf, int32_t len)
{
    if (len <= 0 || !buf) return 0;
    // Pull decoded PCM from the audio pipeline ring buffer
    return audio_pipeline_read_pcm(buf, len);
}

// ---------------------------------------------------------------------------
// GAP callback — device discovery results
// ---------------------------------------------------------------------------
static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        // A device was found during discovery
        if (s_device_count >= BT_MAX_DISCOVERED) break;

        esp_bt_gap_cb_param_t *p = param;

        // Check for duplicate by BDA
        bool duplicate = false;
        for (int d = 0; d < s_device_count; d++) {
            if (memcmp(s_devices[d].bda, p->disc_res.bda, 6) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) break;

        bt_device_t *dev = &s_devices[s_device_count];
        memcpy(dev->bda, p->disc_res.bda, 6);
        dev->name[0] = '\0';
        dev->rssi = 0;

        // Extract name and RSSI from EIR properties
        for (int i = 0; i < p->disc_res.num_prop; i++) {
            esp_bt_gap_dev_prop_t *prop = &p->disc_res.prop[i];
            if (prop->type == ESP_BT_GAP_DEV_PROP_EIR) {
                uint8_t *eir = (uint8_t *)prop->val;
                uint8_t *name_ptr = NULL;
                uint8_t name_len = 0;

                name_ptr = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &name_len);
                if (!name_ptr) {
                    name_ptr = esp_bt_gap_resolve_eir_data(eir, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &name_len);
                }
                if (name_ptr && name_len > 0) {
                    int copy_len = (name_len < sizeof(dev->name) - 1) ? name_len : sizeof(dev->name) - 1;
                    memcpy(dev->name, name_ptr, copy_len);
                    dev->name[copy_len] = '\0';
                }
            } else if (prop->type == ESP_BT_GAP_DEV_PROP_RSSI) {
                dev->rssi = *(int8_t *)prop->val;
            } else if (prop->type == ESP_BT_GAP_DEV_PROP_BDNAME) {
                int copy_len = (prop->len < sizeof(dev->name) - 1) ? prop->len : sizeof(dev->name) - 1;
                memcpy(dev->name, prop->val, copy_len);
                dev->name[copy_len] = '\0';
            }
        }

        // Only list devices that have a name
        if (dev->name[0] != '\0') {
            ESP_LOGI(TAG, "Found [%d]: %s  RSSI=%ld  addr=%02x:%02x:%02x:%02x:%02x:%02x",
                     s_device_count, dev->name, (long)dev->rssi,
                     dev->bda[0], dev->bda[1], dev->bda[2],
                     dev->bda[3], dev->bda[4], dev->bda[5]);
            s_device_count++;
        }
        break;
    }

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            ESP_LOGI(TAG, "Discovery stopped, found %d device(s)", s_device_count);
            if (s_state == BT_STATE_DISCOVERING) {
                s_state = BT_STATE_IDLE;
            }
        } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            ESP_LOGI(TAG, "Discovery started...");
        }
        break;

    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Auth OK with: %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGW(TAG, "Auth FAILED status=%d", param->auth_cmpl.stat);
        }
        break;

    default:
        ESP_LOGD(TAG, "GAP event %d", event);
        break;
    }
}

// ---------------------------------------------------------------------------
// A2DP callback — connection state changes
// ---------------------------------------------------------------------------
static void bt_a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(TAG, "A2DP connected! Checking source ready...");
            s_state = BT_STATE_CONNECTED;
            // Kick off the media start sequence
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGW(TAG, "A2DP disconnected");
            s_state = BT_STATE_IDLE;
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTING) {
            ESP_LOGI(TAG, "A2DP connecting...");
        }
        break;

    case ESP_A2D_AUDIO_STATE_EVT:
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            ESP_LOGI(TAG, "A2DP audio streaming started");
            s_state = BT_STATE_STREAMING;
        } else if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STOPPED ||
                   param->audio_stat.state == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND) {
            ESP_LOGI(TAG, "A2DP audio stopped/suspended");
            if (s_state == BT_STATE_STREAMING) s_state = BT_STATE_CONNECTED;
        }
        break;

    case ESP_A2D_AUDIO_CFG_EVT:
        ESP_LOGI(TAG, "A2DP audio config: codec type=%d", param->audio_cfg.mcc.type);
        break;

    case ESP_A2D_MEDIA_CTRL_ACK_EVT: {
        esp_a2d_media_ctrl_t cmd = param->media_ctrl_stat.cmd;
        esp_a2d_media_ctrl_ack_t status = param->media_ctrl_stat.status;
        ESP_LOGI(TAG, "Media ctrl ACK: cmd=%d status=%d", cmd, status);

        if (cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY) {
            if (status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                ESP_LOGI(TAG, "Source ready, starting media stream...");
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
            } else {
                ESP_LOGW(TAG, "Source not ready, retrying in 500ms...");
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
            }
        } else if (cmd == ESP_A2D_MEDIA_CTRL_START) {
            if (status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                ESP_LOGI(TAG, "Media stream started OK");
            } else {
                ESP_LOGW(TAG, "Media start failed, status=%d", status);
            }
        }
        break;
    }

    default:
        ESP_LOGD(TAG, "A2DP event %d", event);
        break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t bt_a2dp_init(void)
{
    ESP_LOGI(TAG, "Initialising Bluetooth A2DP source");

    // Release BLE memory — we only use Classic
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Set device name
    esp_bt_gap_set_device_name("ESP32 EDM Player");

    // Register GAP and A2DP callbacks
    esp_bt_gap_register_callback(bt_gap_cb);
    esp_a2d_register_callback(bt_a2dp_cb);
    esp_a2d_source_register_data_callback(bt_a2dp_data_cb);
    esp_a2d_source_init();

    // Make device discoverable + connectable (some sinks want to see us too)
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    // Request EIR data so we get device names during discovery
    esp_bt_eir_data_t eir_data = {0};
    esp_bt_gap_config_eir_data(&eir_data);

    ESP_LOGI(TAG, "BT A2DP source ready");
    return ESP_OK;
}

esp_err_t bt_a2dp_start_discovery(void)
{
    s_device_count = 0;
    memset(s_devices, 0, sizeof(s_devices));
    s_state = BT_STATE_DISCOVERING;

    // Discover for 10 seconds (10 * 1.28s = 12.8s)
    esp_err_t ret = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Start discovery failed: %s", esp_err_to_name(ret));
        s_state = BT_STATE_IDLE;
    }
    return ret;
}

esp_err_t bt_a2dp_stop_discovery(void)
{
    return esp_bt_gap_cancel_discovery();
}

esp_err_t bt_a2dp_connect(int device_index)
{
    if (device_index < 0 || device_index >= s_device_count) {
        return ESP_ERR_INVALID_ARG;
    }

    // Stop discovery before connecting
    esp_bt_gap_cancel_discovery();
    vTaskDelay(pdMS_TO_TICKS(500));

    s_state = BT_STATE_CONNECTING;
    bt_device_t *dev = &s_devices[device_index];
    ESP_LOGI(TAG, "Connecting to: %s [%02x:%02x:%02x:%02x:%02x:%02x]",
             dev->name, dev->bda[0], dev->bda[1], dev->bda[2],
             dev->bda[3], dev->bda[4], dev->bda[5]);

    return esp_a2d_source_connect(dev->bda);
}

const bt_device_t *bt_a2dp_get_devices(int *count)
{
    *count = s_device_count;
    return s_devices;
}

bt_state_t bt_a2dp_get_state(void)
{
    return s_state;
}

