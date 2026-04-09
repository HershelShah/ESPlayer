#include "ui_display.h"
#include "config.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display";

// ---------------------------------------------------------------------------
// SPI handle
// ---------------------------------------------------------------------------
static spi_device_handle_t s_spi;

// ---------------------------------------------------------------------------
// Embedded 8x8 bitmap font (printable ASCII 32-126)
// Each character is 8 bytes, one byte per row, MSB = leftmost pixel.
// ---------------------------------------------------------------------------
static const uint8_t font8x8[][8] = {
    // 32 = space
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    // 33 = !
    {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    // 34 = "
    {0x6C,0x6C,0x24,0x00,0x00,0x00,0x00,0x00},
    // 35 = #
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00},
    // 36 = $
    {0x18,0x7E,0xC0,0x7C,0x06,0xFC,0x18,0x00},
    // 37 = %
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00},
    // 38 = &
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00},
    // 39 = '
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
    // 40 = (
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00},
    // 41 = )
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
    // 42 = *
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
    // 43 = +
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    // 44 = ,
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},
    // 45 = -
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    // 46 = .
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
    // 47 = /
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00},
    // 48 = 0
    {0x7C,0xC6,0xCE,0xD6,0xE6,0xC6,0x7C,0x00},
    // 49 = 1
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},
    // 50 = 2
    {0x7C,0xC6,0x06,0x1C,0x30,0x60,0xFE,0x00},
    // 51 = 3
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00},
    // 52 = 4
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x00},
    // 53 = 5
    {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00},
    // 54 = 6
    {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00},
    // 55 = 7
    {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00},
    // 56 = 8
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00},
    // 57 = 9
    {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00},
    // 58 = :
    {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00},
    // 59 = ;
    {0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00},
    // 60 = <
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00},
    // 61 = =
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},
    // 62 = >
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00},
    // 63 = ?
    {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00},
    // 64 = @
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00},
    // 65 = A
    {0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0xC6,0x00},
    // 66 = B
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00},
    // 67 = C
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00},
    // 68 = D
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00},
    // 69 = E
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00},
    // 70 = F
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00},
    // 71 = G
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00},
    // 72 = H
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00},
    // 73 = I
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    // 74 = J
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00},
    // 75 = K
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00},
    // 76 = L
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00},
    // 77 = M
    {0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0x00},
    // 78 = N
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00},
    // 79 = O
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
    // 80 = P
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00},
    // 81 = Q
    {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x06},
    // 82 = R
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00},
    // 83 = S
    {0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00},
    // 84 = T
    {0x7E,0x5A,0x18,0x18,0x18,0x18,0x3C,0x00},
    // 85 = U
    {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
    // 86 = V
    {0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00},
    // 87 = W
    {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00},
    // 88 = X
    {0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00},
    // 89 = Y
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00},
    // 90 = Z
    {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00},
    // 91 = [
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
    // 92 = backslash
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00},
    // 93 = ]
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
    // 94 = ^
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00},
    // 95 = _
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    // 96 = `
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00},
    // 97 = a
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00},
    // 98 = b
    {0xE0,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00},
    // 99 = c
    {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00},
    // 100 = d
    {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00},
    // 101 = e
    {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00},
    // 102 = f
    {0x1C,0x36,0x30,0x78,0x30,0x30,0x78,0x00},
    // 103 = g
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8},
    // 104 = h
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00},
    // 105 = i
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
    // 106 = j
    {0x06,0x00,0x0E,0x06,0x06,0x66,0x66,0x3C},
    // 107 = k
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00},
    // 108 = l
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    // 109 = m
    {0x00,0x00,0xEC,0xFE,0xD6,0xC6,0xC6,0x00},
    // 110 = n
    {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00},
    // 111 = o
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00},
    // 112 = p
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0},
    // 113 = q
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E},
    // 114 = r
    {0x00,0x00,0xDC,0x76,0x60,0x60,0xF0,0x00},
    // 115 = s
    {0x00,0x00,0x7C,0xC0,0x7C,0x06,0xFC,0x00},
    // 116 = t
    {0x30,0x30,0x7C,0x30,0x30,0x36,0x1C,0x00},
    // 117 = u
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00},
    // 118 = v
    {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00},
    // 119 = w
    {0x00,0x00,0xC6,0xC6,0xD6,0xFE,0x6C,0x00},
    // 120 = x
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00},
    // 121 = y
    {0x00,0x00,0xC6,0xC6,0xCE,0x76,0x06,0xFC},
    // 122 = z
    {0x00,0x00,0xFC,0x98,0x30,0x64,0xFC,0x00},
    // 123 = {
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00},
    // 124 = |
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},
    // 125 = }
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00},
    // 126 = ~
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00},
};

// ---------------------------------------------------------------------------
// Low-level SPI helpers
// ---------------------------------------------------------------------------

// Send a command byte (DC low)
static void lcd_cmd(uint8_t cmd)
{
    spi_transaction_t t = {
        .length    = 8,
        .tx_buffer = &cmd,
        .user      = (void *)0,  // DC = 0 (command)
    };
    spi_device_polling_transmit(s_spi, &t);
}

// Send data bytes (DC high)
static void lcd_data(const uint8_t *data, int len)
{
    if (len == 0) return;
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = data,
        .user      = (void *)1,  // DC = 1 (data)
    };
    spi_device_polling_transmit(s_spi, &t);
}

static void lcd_data_byte(uint8_t val)
{
    lcd_data(&val, 1);
}

// SPI pre-transfer callback — sets DC pin based on transaction user field
static void spi_pre_transfer_cb(spi_transaction_t *t)
{
    int dc = (int)t->user;
    gpio_set_level(TFT_DC, dc);
}

// ---------------------------------------------------------------------------
// ILI9341 init sequence
// ---------------------------------------------------------------------------

static void lcd_set_window(int x0, int y0, int x1, int y1)
{
    lcd_cmd(0x2A);  // Column address set
    uint8_t ca[] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
    lcd_data(ca, 4);

    lcd_cmd(0x2B);  // Row address set
    uint8_t ra[] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };
    lcd_data(ra, 4);

    lcd_cmd(0x2C);  // Memory write
}

static void ili9341_init(void)
{
    // Software reset
    lcd_cmd(0x01);
    vTaskDelay(pdMS_TO_TICKS(120));

    // Sleep out
    lcd_cmd(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));

    // Pixel format: 16-bit RGB565
    lcd_cmd(0x3A);
    lcd_data_byte(0x55);

    // Memory access control — landscape, BGR colour order
    // MY=0, MX=0, MV=1 → landscape 320x240, BGR, no X mirror
    lcd_cmd(0x36);
    lcd_data_byte(0x28);

    // Display on
    lcd_cmd(0x29);
    vTaskDelay(pdMS_TO_TICKS(20));
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t ui_display_init(void)
{
    ESP_LOGI(TAG, "Initialising ILI9341 display");

    // Configure DC pin as GPIO output
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << TFT_DC),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Hardware reset if pin is defined
#if TFT_RST >= 0
    io_conf.pin_bit_mask = (1ULL << TFT_RST);
    gpio_config(&io_conf);
    gpio_set_level(TFT_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TFT_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
#endif

    // SPI bus config — SPI2_HOST (HSPI), display pins
    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = TFT_MOSI,
        .miso_io_num   = -1,         // Display has no MISO
        .sclk_io_num   = TFT_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * 16 * 2, // 16 rows at a time
    };
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Attach the ILI9341 device
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 26 * 1000 * 1000,  // 26 MHz — safe for ILI9341
        .mode           = 0,
        .spics_io_num   = TFT_CS,
        .queue_size     = 7,
        .pre_cb         = spi_pre_transfer_cb,
    };
    ret = spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ili9341_init();

    // Backlight — LEDC PWM on GPIO21
    ledc_timer_config_t ledc_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_ch = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .gpio_num   = TFT_BL,
        .duty       = 255,  // Full brightness
        .hpoint     = 0,
    };
    ledc_channel_config(&ledc_ch);

    ESP_LOGI(TAG, "Display ready (320x240 landscape, backlight ON)");
    return ESP_OK;
}

void ui_display_set_backlight(uint8_t brightness)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, brightness);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void ui_display_fill(uint16_t color)
{
    ui_display_fill_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, color);
}

void ui_display_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    lcd_set_window(x, y, x + w - 1, y + h - 1);

    // Send in strips to limit DMA buffer size
    // Each pixel = 2 bytes. Send 1 row at a time.
    uint16_t line_buf[DISPLAY_WIDTH];
    int line_px = (w < DISPLAY_WIDTH) ? w : DISPLAY_WIDTH;
    for (int i = 0; i < line_px; i++) {
        line_buf[i] = __builtin_bswap16(color);  // SPI sends MSB first
    }

    for (int row = 0; row < h; row++) {
        lcd_data((const uint8_t *)line_buf, line_px * 2);
    }
}

void ui_display_char(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = font8x8[c - 32];

    uint16_t buf[8 * 8];  // 128 bytes — fine on stack
    uint16_t fg_swap = __builtin_bswap16(fg);
    uint16_t bg_swap = __builtin_bswap16(bg);

    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            buf[row * 8 + col] = (bits & (0x80 >> col)) ? fg_swap : bg_swap;
        }
    }

    lcd_set_window(x, y, x + 7, y + 7);
    lcd_data((const uint8_t *)buf, sizeof(buf));
}

void ui_display_string(int x, int y, const char *str, uint16_t fg, uint16_t bg)
{
    int cx = x, cy = y;
    while (*str) {
        if (*str == '\n') {
            cx = x;
            cy += 10;  // 8px glyph + 2px spacing
        } else {
            if (cx + 8 > DISPLAY_WIDTH) {
                cx = x;
                cy += 10;
            }
            ui_display_char(cx, cy, *str, fg, bg);
            cx += 8;
        }
        str++;
    }
}

// Draw a 2x-scaled character (16x16) for title text
static void draw_char_2x(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = font8x8[c - 32];

    uint16_t buf[16 * 16];  // 512 bytes on stack
    uint16_t fg_swap = __builtin_bswap16(fg);
    uint16_t bg_swap = __builtin_bswap16(bg);

    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            uint16_t px = (bits & (0x80 >> col)) ? fg_swap : bg_swap;
            int idx0 = (row * 2) * 16 + col * 2;
            int idx1 = (row * 2 + 1) * 16 + col * 2;
            buf[idx0]     = px;
            buf[idx0 + 1] = px;
            buf[idx1]     = px;
            buf[idx1 + 1] = px;
        }
    }

    lcd_set_window(x, y, x + 15, y + 15);
    lcd_data((const uint8_t *)buf, sizeof(buf));
}

static void draw_string_2x(int x, int y, const char *str, uint16_t fg, uint16_t bg)
{
    while (*str) {
        draw_char_2x(x, y, *str, fg, bg);
        x += 16;
        str++;
    }
}

void ui_display_splash(int track_count)
{
    // Dark background
    ui_display_fill(COLOR_BLACK);

    // Title — 2x scaled, centred
    const char *title = "ESP32 EDM Player";
    int title_w = (int)strlen(title) * 16;
    int title_x = (DISPLAY_WIDTH - title_w) / 2;
    draw_string_2x(title_x, 40, title, COLOR_CYAN, COLOR_BLACK);

    // Subtitle
    const char *sub = "CYD Prototype";
    int sub_w = (int)strlen(sub) * 8;
    int sub_x = (DISPLAY_WIDTH - sub_w) / 2;
    ui_display_string(sub_x, 70, sub, COLOR_PURPLE, COLOR_BLACK);

    // Divider line
    ui_display_fill_rect(40, 100, DISPLAY_WIDTH - 80, 2, COLOR_PURPLE);

    // Track count
    char info[48];
    snprintf(info, sizeof(info), "%d tracks on SD card", track_count);
    int info_w = (int)strlen(info) * 8;
    int info_x = (DISPLAY_WIDTH - info_w) / 2;
    ui_display_string(info_x, 120, info, COLOR_WHITE, COLOR_BLACK);

    // Heap info
    extern size_t heap_caps_get_free_size(uint32_t caps);
    size_t free_heap = heap_caps_get_free_size(0x00002000);  // MALLOC_CAP_INTERNAL
    snprintf(info, sizeof(info), "Free heap: %u bytes", (unsigned)free_heap);
    info_w = (int)strlen(info) * 8;
    info_x = (DISPLAY_WIDTH - info_w) / 2;
    ui_display_string(info_x, 140, info, COLOR_GREEN, COLOR_BLACK);

    // Status
    const char *status = "Step 2: Display OK";
    int st_w = (int)strlen(status) * 8;
    int st_x = (DISPLAY_WIDTH - st_w) / 2;
    ui_display_string(st_x, 180, status, COLOR_YELLOW, COLOR_BLACK);
}
