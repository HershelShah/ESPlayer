#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int x;   // Display coordinate 0..319
    int y;   // Display coordinate 0..239
} touch_point_t;

// Initialise XPT2046 touch controller (bit-bang SPI).
esp_err_t ui_touch_init(void);

// Returns true if the screen is currently being touched.
bool ui_touch_pressed(void);

// Read the current touch position mapped to display coordinates.
// Returns true if a valid touch was read; false if not touched.
bool ui_touch_read(touch_point_t *pt);

// Read raw ADC values (for calibration). Returns true if touched.
bool ui_touch_read_raw(int *raw_x, int *raw_y);
