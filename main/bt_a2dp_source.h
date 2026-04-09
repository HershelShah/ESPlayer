#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// Maximum number of discovered devices shown to user
#define BT_MAX_DISCOVERED  10

typedef struct {
    uint8_t  bda[6];       // Bluetooth device address
    char     name[64];     // Device name (from EIR)
    int32_t  rssi;         // Signal strength
} bt_device_t;

typedef enum {
    BT_STATE_IDLE,
    BT_STATE_DISCOVERING,
    BT_STATE_CONNECTING,
    BT_STATE_CONNECTED,
    BT_STATE_STREAMING,
} bt_state_t;

// Initialise Bluetooth stack and A2DP source profile.
esp_err_t bt_a2dp_init(void);

// Start scanning for A2DP sink devices. Results arrive asynchronously.
esp_err_t bt_a2dp_start_discovery(void);

// Stop an in-progress discovery.
esp_err_t bt_a2dp_stop_discovery(void);

// Connect to a discovered device by index.
esp_err_t bt_a2dp_connect(int device_index);

// Get list of discovered devices and count.
const bt_device_t *bt_a2dp_get_devices(int *count);

// Get current BT state.
bt_state_t bt_a2dp_get_state(void);

