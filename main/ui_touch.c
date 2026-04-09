#include "ui_touch.h"
#include "config.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "rom/ets_sys.h"   // ets_delay_us

static const char *TAG = "touch";

// ---------------------------------------------------------------------------
// Calibration constants — raw ADC range mapped to display pixels.
// These are typical for CYD boards in landscape (MADCTL 0x28).
// Fine-tune with ui_touch_read_raw() output if needed.
// ---------------------------------------------------------------------------
#define RAW_X_MIN   200
#define RAW_X_MAX   3900
#define RAW_Y_MIN   200
#define RAW_Y_MAX   3900

// XPT2046 command bytes (12-bit, differential reference mode)
#define CMD_READ_X  0xD0
#define CMD_READ_Y  0x90

// ---------------------------------------------------------------------------
// Bit-bang SPI helpers
// ---------------------------------------------------------------------------

static inline void clk_high(void)  { gpio_set_level(TOUCH_CLK, 1); }
static inline void clk_low(void)   { gpio_set_level(TOUCH_CLK, 0); }
static inline void cs_high(void)   { gpio_set_level(TOUCH_CS, 1); }
static inline void cs_low(void)    { gpio_set_level(TOUCH_CS, 0); }
static inline void mosi_set(int v) { gpio_set_level(TOUCH_MOSI, v); }
static inline int  miso_get(void)  { return gpio_get_level(TOUCH_MISO); }

// ~1 us half-clock — plenty fast for XPT2046 (max ~2 MHz)
static inline void spi_delay(void) { ets_delay_us(1); }

static void spi_write_byte(uint8_t data)
{
    for (int i = 7; i >= 0; i--) {
        mosi_set((data >> i) & 1);
        spi_delay();
        clk_high();
        spi_delay();
        clk_low();
    }
}

static uint16_t spi_read_12bit(void)
{
    uint16_t val = 0;
    for (int i = 11; i >= 0; i--) {
        clk_high();
        spi_delay();
        val |= (miso_get() << i);
        clk_low();
        spi_delay();
    }
    return val;
}

static uint16_t xpt2046_read_channel(uint8_t cmd)
{
    cs_low();
    spi_write_byte(cmd);
    // One busy cycle
    clk_high(); spi_delay(); clk_low(); spi_delay();
    uint16_t val = spi_read_12bit();
    cs_high();
    return val;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t ui_touch_init(void)
{
    ESP_LOGI(TAG, "Initialising XPT2046 touch (bit-bang SPI)");

    // Output pins: CS, CLK, MOSI
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << TOUCH_CS) | (1ULL << TOUCH_CLK) | (1ULL << TOUCH_MOSI),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);

    // Input pins: MISO, IRQ
    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << TOUCH_MISO) | (1ULL << TOUCH_IRQ),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_cfg);

    // Idle state
    cs_high();
    clk_low();

    ESP_LOGI(TAG, "Touch ready (IRQ=GPIO%d)", TOUCH_IRQ);
    return ESP_OK;
}

bool ui_touch_pressed(void)
{
    // IRQ pin is active LOW when screen is touched
    return gpio_get_level(TOUCH_IRQ) == 0;
}

bool ui_touch_read_raw(int *raw_x, int *raw_y)
{
    if (!ui_touch_pressed()) return false;

    // Average multiple samples for noise reduction
    uint32_t sum_x = 0, sum_y = 0;
    const int samples = 4;

    for (int i = 0; i < samples; i++) {
        sum_x += xpt2046_read_channel(CMD_READ_X);
        sum_y += xpt2046_read_channel(CMD_READ_Y);
    }

    *raw_x = sum_x / samples;
    *raw_y = sum_y / samples;

    // Reject bogus readings (finger lifted mid-read)
    if (*raw_x < 100 || *raw_y < 100) return false;

    return true;
}

bool ui_touch_read(touch_point_t *pt)
{
    int raw_x, raw_y;
    if (!ui_touch_read_raw(&raw_x, &raw_y)) return false;

    // Map raw ADC to display coordinates (landscape, MADCTL 0x28)
    // CYD touch panel axes are swapped relative to display in landscape:
    // raw_y → display X, raw_x → display Y
    int dx = (raw_y - RAW_Y_MIN) * DISPLAY_WIDTH  / (RAW_Y_MAX - RAW_Y_MIN);
    int dy = (raw_x - RAW_X_MIN) * DISPLAY_HEIGHT / (RAW_X_MAX - RAW_X_MIN);

    // Clamp
    if (dx < 0) dx = 0;
    if (dx >= DISPLAY_WIDTH)  dx = DISPLAY_WIDTH - 1;
    if (dy < 0) dy = 0;
    if (dy >= DISPLAY_HEIGHT) dy = DISPLAY_HEIGHT - 1;

    pt->x = dx;
    pt->y = dy;
    return true;
}
