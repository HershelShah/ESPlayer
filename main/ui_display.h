#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// RGB565 colour helpers
#define RGB565(r, g, b) ((uint16_t)(((r) & 0xF8) << 8 | ((g) & 0xFC) << 3 | ((b) >> 3)))

#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_CYAN    0x07FF
#define COLOR_MAGENTA 0xF81F
#define COLOR_YELLOW  0xFFE0
#define COLOR_ORANGE  RGB565(255, 165, 0)
#define COLOR_PURPLE  RGB565(128, 0, 255)

// Initialise ILI9341 display + backlight. Returns ESP_OK on success.
esp_err_t ui_display_init(void);

// Fill entire screen with a colour.
void ui_display_fill(uint16_t color);

// Draw a filled rectangle.
void ui_display_fill_rect(int x, int y, int w, int h, uint16_t color);

// Draw a single character at (x,y) using the built-in 8x8 font.
void ui_display_char(int x, int y, char c, uint16_t fg, uint16_t bg);

// Draw a null-terminated string. Wraps at screen edge.
void ui_display_string(int x, int y, const char *str, uint16_t fg, uint16_t bg);

// Draw the splash/boot screen.
void ui_display_splash(int track_count);

// Set backlight brightness (0-255). 0 = off, 255 = full.
void ui_display_set_backlight(uint8_t brightness);
