#pragma once

// ============================================================
// ESP32 EDM Player — Hardware Pin Definitions & Constants
// Board: CYD ESP32-2432S028R (ESP32-WROOM-32, no PSRAM)
// ============================================================

// --- TFT Display (ILI9341, HSPI) ---
#define TFT_MOSI        13
#define TFT_SCLK        14
#define TFT_CS          15
#define TFT_DC           2
#define TFT_RST         (-1)   // Try -1 first; use 12 if display doesn't init
#define TFT_BL          21     // Backlight, active HIGH, PWM dimmable

// --- Touchscreen (XPT2046, dedicated SPI) ---
#define TOUCH_CS        33
#define TOUCH_CLK       25
#define TOUCH_MOSI      32
#define TOUCH_MISO      39
#define TOUCH_IRQ       36

// --- SD Card (SPI bus — separate from display SPI) ---
#define SD_CS            5
#define SD_MOSI         23
#define SD_MISO         19
#define SD_SCK          18

// --- Speaker (internal 8-bit DAC) ---
#define SPEAKER_OUT     26

// --- RGB LED ---
#define RGB_RED          4
#define RGB_GREEN       16
#define RGB_BLUE        17

// --- Sensors & Buttons ---
#define LDR_PIN         34     // Light sensor (ADC)
#define BOOT_BTN         0

// --- Audio Buffer Sizes ---
#define SD_READ_BUF_SIZE    4096    // 4KB — ~10 MP3 frames
#define PCM_RINGBUF_SIZE   32768    // 32KB — ~0.18s of 44.1kHz stereo 16-bit

// --- Audio Constants ---
#define AUDIO_SAMPLE_RATE  44100
#define AUDIO_CHANNELS     2       // Stereo
#define AUDIO_BITS         16

// --- UI Constants ---
#define DISPLAY_WIDTH   320
#define DISPLAY_HEIGHT  240

// --- Backlight Timeouts (ms) ---
#define BL_DIM_TIMEOUT_MS    15000   // Dim after 15s no touch
#define BL_OFF_TIMEOUT_MS    60000   // Off after 60s no touch

// --- SD Card ---
#define SD_MOUNT_POINT  "/sdcard"
#define MUSIC_DIR       "/sdcard/music"
#define MAX_TRACKS      100

// --- Heap Safety ---
#define HEAP_MIN_FREE   (30 * 1024)  // 30KB — never drop below this
