// SSD1677 driver for the Metalio E-Ink 4 panel (GDEM0397T81, 800x480).
//
// Ported to plain C from:
//  - Vendor firmware (MIT, Copyright 2026 CloudZao), https://github.com/CloudZao/Metalio-E-INK4 @ d12f481
//      main/boards/metalio-e-ink-4/display/esp_lcd_panel_ssd1677.c   (init, LUT load, refresh, BUSY)
//      main/boards/metalio-e-ink-4/display/esp_lcd_ssd1677_commands.h (command / parameter values)
//      main/boards/metalio-e-ink-4/display/metalio_epd.cc            (SPI setup, reset)
//      main/boards/metalio-e-ink-4/config.h                          (pins, 10 MHz)
//      main/display/LUT/epd_lut_fastupdate.h, epd_lut_partial.h, epd_lut_4gray.h (waveforms)
//      main/display/lv_adapter_display.cc                            (plane usage per refresh, gray overlay)
//      main/display/epd_gray_aa.cc                                   (overlay plane encoding)
//    Some of those files carry an "SPDX-License-Identifier: Apache-2.0" header; the repository LICENSE is MIT.
//  - FreeInk SDK (MIT, Copyright 2026 FreeInk), https://github.com/Free-Ink/freeink-sdk @ e637713
//      libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp (per-mode border, plane resync, OTP full 0xF7,
//      power-off before deep sleep)
//      docs/metalio-eink4-support.md                            (pins, BUSY active high)
#include "kiban/epd.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifndef CONFIG_KIBAN_EPD_BUSY_TIMEOUT_MS
#define CONFIG_KIBAN_EPD_BUSY_TIMEOUT_MS 5000
#endif
#ifndef CONFIG_KIBAN_EPD_SPI_HZ
#define CONFIG_KIBAN_EPD_SPI_HZ 10000000
#endif

static const char *TAG = "kiban.epd";

// ---- SSD1677 commands [vendor esp_lcd_ssd1677_commands.h] ----
#define CMD_DRIVER_OUTPUT   0x01
#define CMD_GATE_VOLTAGE    0x03
#define CMD_SOURCE_VOLTAGE  0x04
#define CMD_BOOSTER         0x0C
#define CMD_DEEP_SLEEP      0x10
#define CMD_DATA_ENTRY      0x11
#define CMD_SWRESET         0x12
#define CMD_TEMP_SENSOR     0x18
#define CMD_WRITE_TEMP      0x1A
#define CMD_ACTIVATE        0x20
#define CMD_UPDATE_CTRL1    0x21
#define CMD_UPDATE_CTRL2    0x22
#define CMD_RAM_BW          0x24   // "new" frame
#define CMD_RAM_RED         0x26   // "old" frame
#define CMD_VCOM            0x2C
#define CMD_LUT             0x32
#define CMD_DISPLAY_OPTION  0x37
#define CMD_BORDER          0x3C
#define CMD_END_OPTION      0x3F
#define CMD_RAM_X_RANGE     0x44
#define CMD_RAM_Y_RANGE     0x45
#define CMD_RAM_X_COUNTER   0x4E
#define CMD_RAM_Y_COUNTER   0x4F

// 0x22 Display Update Control 2 values [vendor commands.h:69-86, non-"OPT" path which is the active one]
#define CTRL2_FULL          0xC7   // clock+analog on, display mode 1 with the loaded LUT, power off
#define CTRL2_PARTIAL       0xCF   // same, display mode 2
#define CTRL2_GRAY          0xCC   // clock+analog on, display mode 2, stays powered
#define CTRL2_FAST          0xD7   // load LUT from OTP (forced temperature), mode 1, power off
#define CTRL2_FULL_OTP      0xF7   // load temperature + OTP LUT, mode 1, power off [FreeInk Ssd1677Driver.cpp:130]
#define CTRL2_POWER_OFF     0x83   // [vendor commands.h:86]

#define BORDER_FULL         0x01   // [vendor commands.h:37] init value; FreeInk Metalio borderWaveformFull
#define BORDER_PARTIAL      0x80   // [vendor esp_lcd_panel_ssd1677.c:565-575] "floating/VCOM" before partial

// ---- Waveforms. Each table: [0..104] -> 0x32, [105] -> 0x03 (VGH), [106..108] -> 0x04 (VSH1/VSH2/VSL),
// [109] -> 0x2C (VCOM). Copied verbatim from the active (#if 1) branch of the vendor headers. ----
#define LUT_WAVE_BYTES 105
#define LUT_BYTES      110

// Vendor full-refresh LUT [vendor main/display/LUT/epd_lut_fastupdate.h:12-31]
static const uint8_t LUT_FULL[LUT_BYTES] = {
    0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x14, 0x14, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02,
    0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x22, 0x22, 0x22, 0x22, 0x22,
    0x17, 0x41, 0xBA, 0x32, 0x50,
};

// Vendor partial LUT [vendor main/display/LUT/epd_lut_partial.h:12-31]
static const uint8_t LUT_PARTIAL[LUT_BYTES] = {
    0x00, 0xCC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0D, 0x00, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x22, 0x22, 0x22, 0x22, 0x22,
    0x17, 0x41, 0xBA, 0x32, 0x00,
};

// Vendor 4-gray overlay LUT [vendor main/display/LUT/epd_lut_4gray.h:11-30]
// (same waveform as FreeInk lut_grayscale; FreeInk's Metalio copy raises VSH1 to 0x46)
static const uint8_t LUT_GRAY[LUT_BYTES] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x54, 0x54, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xAA, 0xA0, 0xA8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xA2, 0x22, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x01, 0x01, 0x01, 0x00,
    0x01, 0x01, 0x01, 0x01, 0x00,
    0x01, 0x01, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x8F, 0x8F, 0x8F, 0x8F, 0x8F,
    0x17, 0x41, 0xA8, 0x32, 0x30,
};

typedef enum { LUT_NONE = 0, LUT_IS_FULL, LUT_IS_PARTIAL, LUT_IS_GRAY } loaded_lut_t;

// Bounce buffer: 40 rows of a 1bpp frame. Every byte sent goes through it, so callers' frames can
// live anywhere (the vendor copies PSRAM frames to internal DMA memory too, esp_lcd_panel_ssd1677.c:481-496).
#define ROW_BYTES   (KIBAN_EPD_WIDTH / 8)
#define CHUNK_ROWS  40
#define CHUNK_BYTES (ROW_BYTES * CHUNK_ROWS)

static struct {
    bool bus_up;
    bool ready;            // init done and no timeout since
    bool have_base;        // both RAM planes hold the frame on the glass
    bool analog_on;        // last update left clock/analog on (0xCC)
    loaded_lut_t lut;
    spi_device_handle_t dev;
    uint8_t *bounce;       // CHUNK_BYTES, internal DMA-capable
    kiban_epd_stats_t stats;
} s;

// ---------------------------------------------------------------- SPI

static esp_err_t spi_send(const uint8_t *data, size_t len)
{
    spi_transaction_t t = {.length = len * 8, .tx_buffer = data};
    return spi_device_transmit(s.dev, &t);
}

static esp_err_t send_cmd_byte(uint8_t c)
{
    gpio_set_level(KIBAN_EPD_PIN_DC, 0);
    spi_transaction_t t = {.length = 8, .flags = SPI_TRANS_USE_TXDATA, .tx_data = {c}};
    return spi_device_transmit(s.dev, &t);
}

// Command plus small parameter block (<= CHUNK_BYTES), with CS framing.
static esp_err_t cmd(uint8_t c, const uint8_t *param, size_t n)
{
    gpio_set_level(KIBAN_EPD_PIN_CS, 0);
    esp_err_t err = send_cmd_byte(c);
    if (err == ESP_OK && n > 0) {
        memcpy(s.bounce, param, n);
        gpio_set_level(KIBAN_EPD_PIN_DC, 1);
        err = spi_send(s.bounce, n);
    }
    gpio_set_level(KIBAN_EPD_PIN_CS, 1);
    if (err != ESP_OK) ESP_LOGE(TAG, "cmd 0x%02x: %s", c, esp_err_to_name(err));
    return err;
}

#define CMD1(c, v) cmd((c), (const uint8_t[]){(v)}, 1)

// Plane producers fill the bounce buffer with `rows` rows starting at `row`.
typedef void (*plane_fill_t)(uint8_t *dst, const uint8_t *src, int row, int rows);

static void fill_copy(uint8_t *dst, const uint8_t *src, int row, int rows)
{
    memcpy(dst, src + (size_t)row * ROW_BYTES, (size_t)rows * ROW_BYTES);
}

// 2bpp -> 1bpp. Overlay encoding [vendor epd_gray_aa.cc:92-119 + epd_i1_glyph_thin.h:33-43]:
//   base (0x24/0x26 for the B/W pass): every non-white pixel black ("non-white is black" I1 base);
//   LSB (0x24 for the gray pass): dark gray only;  MSB (0x26 for the gray pass): dark or light gray.
static inline uint8_t gray_px(const uint8_t *src2, size_t i) { return (src2[i >> 2] >> (6 - 2 * (i & 3))) & 3; }

static void fill_gray(uint8_t *dst, const uint8_t *src2, int row, int rows, int which)
{
    for (int r = 0; r < rows; r++) {
        const size_t base = (size_t)(row + r) * KIBAN_EPD_WIDTH;
        for (int b = 0; b < ROW_BYTES; b++) {
            uint8_t out = 0;
            for (int k = 0; k < 8; k++) {
                const uint8_t v = gray_px(src2, base + (size_t)b * 8 + k);
                int bit;
                switch (which) {
                case 0:  bit = (v == 3); break;            // base: white stays white (1)
                case 1:  bit = (v == 1); break;            // LSB: dark gray
                default: bit = (v == 1 || v == 2); break;  // MSB: dark or light gray
                }
                out |= (uint8_t)(bit << (7 - k));
            }
            dst[r * ROW_BYTES + b] = out;
        }
    }
}
static void fill_gray_base(uint8_t *d, const uint8_t *s2, int row, int rows) { fill_gray(d, s2, row, rows, 0); }
static void fill_gray_lsb(uint8_t *d, const uint8_t *s2, int row, int rows) { fill_gray(d, s2, row, rows, 1); }
static void fill_gray_msb(uint8_t *d, const uint8_t *s2, int row, int rows) { fill_gray(d, s2, row, rows, 2); }

// Full-window RAM area, X increment / Y decrement; the gate order is reversed on this panel, so frame
// row 0 goes to gate address 479. [vendor esp_lcd_panel_ssd1677.c:115-142, data entry 0x01]
static esp_err_t set_ram_area_full(void)
{
    const uint16_t x_end = KIBAN_EPD_WIDTH - 1, y_end = KIBAN_EPD_HEIGHT - 1;
    ESP_RETURN_ON_ERROR(CMD1(CMD_DATA_ENTRY, 0x01), TAG, "data entry");
    ESP_RETURN_ON_ERROR(cmd(CMD_RAM_X_RANGE, (const uint8_t[]){0, 0, x_end & 0xFF, x_end >> 8}, 4), TAG, "x range");
    ESP_RETURN_ON_ERROR(cmd(CMD_RAM_Y_RANGE, (const uint8_t[]){y_end & 0xFF, y_end >> 8, 0, 0}, 4), TAG, "y range");
    ESP_RETURN_ON_ERROR(cmd(CMD_RAM_X_COUNTER, (const uint8_t[]){0, 0}, 2), TAG, "x counter");
    ESP_RETURN_ON_ERROR(cmd(CMD_RAM_Y_COUNTER, (const uint8_t[]){y_end & 0xFF, y_end >> 8}, 2), TAG, "y counter");
    return ESP_OK;
}

// Writes one whole RAM plane (0x24 or 0x26) produced chunk by chunk from src.
static esp_err_t write_plane(uint8_t ram_cmd, const uint8_t *src, plane_fill_t fill)
{
    ESP_RETURN_ON_ERROR(set_ram_area_full(), TAG, "ram area");
    gpio_set_level(KIBAN_EPD_PIN_CS, 0);
    esp_err_t err = send_cmd_byte(ram_cmd);
    gpio_set_level(KIBAN_EPD_PIN_DC, 1);
    for (int row = 0; err == ESP_OK && row < KIBAN_EPD_HEIGHT; row += CHUNK_ROWS) {
        fill(s.bounce, src, row, CHUNK_ROWS);
        err = spi_send(s.bounce, CHUNK_BYTES);
    }
    gpio_set_level(KIBAN_EPD_PIN_CS, 1);
    if (err != ESP_OK) ESP_LOGE(TAG, "ram 0x%02x write: %s", ram_cmd, esp_err_to_name(err));
    return err;
}

// ---------------------------------------------------------------- BUSY

// BUSY is high while the controller works [vendor esp_lcd_panel_ssd1677.c:145-153; FreeInk
// metalio-eink4-support.md:53]. The vendor loop has no timeout; this one does. Resolution: one tick.
static esp_err_t wait_busy(uint32_t timeout_ms, const char *what, uint32_t *elapsed_ms, bool *seen)
{
    const int64_t t0 = esp_timer_get_time();
    const int64_t deadline = t0 + (int64_t)timeout_ms * 1000;
    bool high = false;
    // Short spin first so a BUSY pulse that starts a little late is still observed.
    for (int i = 0; i < 20 && !gpio_get_level(KIBAN_EPD_PIN_BUSY); i++) esp_rom_delay_us(100);
    while (gpio_get_level(KIBAN_EPD_PIN_BUSY)) {
        high = true;
        if (esp_timer_get_time() > deadline) {
            ESP_LOGE(TAG, "BUSY still high after %lu ms (%s)", (unsigned long)timeout_ms, what);
            s.ready = false;
            s.have_base = false;
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    esp_rom_delay_us(100);   // vendor settles 100 us after BUSY falls
    if (elapsed_ms) *elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (seen) *seen = high;
    return ESP_OK;
}

static uint32_t eff_timeout(uint32_t t) { return t ? t : CONFIG_KIBAN_EPD_BUSY_TIMEOUT_MS; }

// ---------------------------------------------------------------- LUT

// Order VCOM -> gate -> source -> 0x32, then BUSY [vendor esp_lcd_panel_ssd1677.c:348-372].
static esp_err_t load_lut(const uint8_t *lut, loaded_lut_t id)
{
    if (s.lut == id) return ESP_OK;
    ESP_RETURN_ON_ERROR(CMD1(CMD_VCOM, lut[109]), TAG, "vcom");
    ESP_RETURN_ON_ERROR(CMD1(CMD_GATE_VOLTAGE, lut[105]), TAG, "vgh");
    ESP_RETURN_ON_ERROR(cmd(CMD_SOURCE_VOLTAGE, &lut[106], 3), TAG, "vsh/vsl");
    ESP_RETURN_ON_ERROR(cmd(CMD_LUT, lut, LUT_WAVE_BYTES), TAG, "lut");
    s.lut = LUT_NONE;
    ESP_RETURN_ON_ERROR(wait_busy(CONFIG_KIBAN_EPD_BUSY_TIMEOUT_MS, "lut load", NULL, NULL), TAG, "lut busy");
    s.lut = id;
    return ESP_OK;
}

// ---------------------------------------------------------------- refresh

static esp_err_t activate(uint8_t ctrl2, uint32_t timeout_ms, const char *what)
{
    ESP_RETURN_ON_ERROR(CMD1(CMD_UPDATE_CTRL2, ctrl2), TAG, "ctrl2");
    ESP_RETURN_ON_ERROR(cmd(CMD_ACTIVATE, NULL, 0), TAG, "activate");
    s.stats.busy_ms = 0;
    s.stats.busy_seen = false;
    ESP_RETURN_ON_ERROR(wait_busy(eff_timeout(timeout_ms), what, &s.stats.busy_ms, &s.stats.busy_seen), TAG, "%s", what);
    s.analog_on = (ctrl2 & 0x03) == 0;
    if (!s.stats.busy_seen) ESP_LOGW(TAG, "%s: BUSY never went high (refresh may not have run)", what);
    ESP_LOGI(TAG, "%s: 0x22=0x%02X transfer %lu ms, BUSY %lu ms", what, ctrl2, (unsigned long)s.stats.transfer_ms,
             (unsigned long)s.stats.busy_ms);
    return ESP_OK;
}

// MCU-LUT full refresh of whatever both planes hold [vendor esp_lcd_panel_ssd1677.c:163-234].
static esp_err_t refresh_full(uint32_t timeout_ms, const char *what)
{
    ESP_RETURN_ON_ERROR(CMD1(CMD_BORDER, BORDER_FULL), TAG, "border");
    ESP_RETURN_ON_ERROR(load_lut(LUT_FULL, LUT_IS_FULL), TAG, "full lut");
    ESP_RETURN_ON_ERROR(activate(CTRL2_FULL, timeout_ms, what), TAG, "full");
    // Non-OPT vendor path writes 0x3F=0x01 after BUSY on synchronous refreshes [.c:227-232].
    return CMD1(CMD_END_OPTION, 0x01);
}

esp_err_t kiban_epd_show(const uint8_t *fb, kiban_epd_mode_t mode, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(fb, ESP_ERR_INVALID_ARG, TAG, "fb is NULL");
    ESP_RETURN_ON_FALSE(s.ready, ESP_ERR_INVALID_STATE, TAG, "not initialised (call kiban_epd_init)");
    ESP_RETURN_ON_FALSE(mode != KIBAN_EPD_MODE_PARTIAL || s.have_base, ESP_ERR_INVALID_STATE, TAG,
                        "PARTIAL needs a previous refresh since init");

    const int64_t t0 = esp_timer_get_time();
    if (mode == KIBAN_EPD_MODE_PARTIAL) {
        // Only the new frame goes to 0x24; 0x26 already holds the old one [vendor lv_adapter_display.cc:848-869].
        ESP_RETURN_ON_ERROR(write_plane(CMD_RAM_BW, fb, fill_copy), TAG, "0x24");
    } else {
        // Full refreshes write the frame to both planes [vendor lv_adapter_display.cc:913-927].
        ESP_RETURN_ON_ERROR(write_plane(CMD_RAM_RED, fb, fill_copy), TAG, "0x26");
        ESP_RETURN_ON_ERROR(write_plane(CMD_RAM_BW, fb, fill_copy), TAG, "0x24");
    }
    s.stats.transfer_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    s.have_base = false;

    switch (mode) {
    case KIBAN_EPD_MODE_FULL:
        ESP_RETURN_ON_ERROR(refresh_full(timeout_ms, "full"), TAG, "full");
        break;
    case KIBAN_EPD_MODE_FAST:
        // [vendor esp_lcd_panel_ssd1677.c:178-182] 0x1A=0x6A then 0xD7. 0xD7 reloads the LUT (and
        // voltages) from OTP, so the cached MCU LUT is no longer valid afterwards.
        ESP_RETURN_ON_ERROR(CMD1(CMD_BORDER, BORDER_FULL), TAG, "border");
        ESP_RETURN_ON_ERROR(CMD1(CMD_WRITE_TEMP, 0x6A), TAG, "temp");
        s.lut = LUT_NONE;
        ESP_RETURN_ON_ERROR(activate(CTRL2_FAST, timeout_ms, "fast"), TAG, "fast");
        ESP_RETURN_ON_ERROR(CMD1(CMD_END_OPTION, 0x01), TAG, "end option");
        break;
    case KIBAN_EPD_MODE_FULL_OTP:
        // [FreeInk Ssd1677Driver.cpp:127-133,503-526] Metalio full: border 0x01, 0x22=0xF7.
        ESP_RETURN_ON_ERROR(CMD1(CMD_BORDER, BORDER_FULL), TAG, "border");
        ESP_RETURN_ON_ERROR(CMD1(CMD_TEMP_SENSOR, 0x80), TAG, "temp sensor");
        s.lut = LUT_NONE;
        ESP_RETURN_ON_ERROR(activate(CTRL2_FULL_OTP, timeout_ms, "full_otp"), TAG, "full otp");
        break;
    case KIBAN_EPD_MODE_PARTIAL:
        // [vendor esp_lcd_panel_ssd1677.c:565-575, 213-217, 236-239] border 0x80, partial LUT, 0xCF.
        ESP_RETURN_ON_ERROR(CMD1(CMD_BORDER, BORDER_PARTIAL), TAG, "border");
        ESP_RETURN_ON_ERROR(load_lut(LUT_PARTIAL, LUT_IS_PARTIAL), TAG, "partial lut");
        ESP_RETURN_ON_ERROR(activate(CTRL2_PARTIAL, timeout_ms, "partial"), TAG, "partial");
        // Resync 0x26 to the displayed frame (FreeInk syncs both planes after each refresh,
        // Ssd1677Driver.cpp:750-759). The vendor instead relies on the 0x37 ping-pong option; writing
        // the plane makes the next PARTIAL correct either way.
        ESP_RETURN_ON_ERROR(write_plane(CMD_RAM_RED, fb, fill_copy), TAG, "0x26 resync");
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    s.have_base = true;
    return ESP_OK;
}

esp_err_t kiban_epd_show_gray(const uint8_t *fb2, uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(fb2, ESP_ERR_INVALID_ARG, TAG, "fb is NULL");
    ESP_RETURN_ON_FALSE(s.ready, ESP_ERR_INVALID_STATE, TAG, "not initialised (call kiban_epd_init)");

    // 1) B/W base, full refresh [vendor lv_adapter_display.cc:913-927,976 FullRefreshBothSame].
    int64_t t0 = esp_timer_get_time();
    s.have_base = false;
    ESP_RETURN_ON_ERROR(write_plane(CMD_RAM_RED, fb2, fill_gray_base), TAG, "base 0x26");
    ESP_RETURN_ON_ERROR(write_plane(CMD_RAM_BW, fb2, fill_gray_base), TAG, "base 0x24");
    s.stats.transfer_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    ESP_RETURN_ON_ERROR(refresh_full(timeout_ms, "gray base"), TAG, "gray base");

    // 2) Overlay: LSB -> 0x24, MSB -> 0x26, gray LUT, CTRL1 normal, 0xCC
    //    [vendor lv_adapter_display.cc:929-959 GrayOverlayAfterBwBase; esp_lcd_panel_ssd1677.c:201-208].
    t0 = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(write_plane(CMD_RAM_BW, fb2, fill_gray_lsb), TAG, "lsb 0x24");
    ESP_RETURN_ON_ERROR(write_plane(CMD_RAM_RED, fb2, fill_gray_msb), TAG, "msb 0x26");
    s.stats.transfer_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    ESP_RETURN_ON_ERROR(load_lut(LUT_GRAY, LUT_IS_GRAY), TAG, "gray lut");
    ESP_RETURN_ON_ERROR(CMD1(CMD_UPDATE_CTRL1, 0x00), TAG, "ctrl1");
    ESP_RETURN_ON_ERROR(activate(CTRL2_GRAY, timeout_ms, "gray"), TAG, "gray");
    ESP_RETURN_ON_ERROR(CMD1(CMD_END_OPTION, 0x01), TAG, "end option");   // vendor: GRAY is a full_sync mode

    // 3) Write the B/W base back to both planes [vendor lv_adapter_display.cc:957].
    ESP_RETURN_ON_ERROR(write_plane(CMD_RAM_RED, fb2, fill_gray_base), TAG, "restore 0x26");
    ESP_RETURN_ON_ERROR(write_plane(CMD_RAM_BW, fb2, fill_gray_base), TAG, "restore 0x24");
    s.have_base = true;
    return ESP_OK;
}

// ---------------------------------------------------------------- lifecycle

static esp_err_t hw_reset(void)
{
    // [vendor esp_lcd_panel_ssd1677.c:333-346, metalio_epd.cc:71-73] RST low 10 ms, high 10 ms, BUSY, 100 ms.
    gpio_set_level(KIBAN_EPD_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(KIBAN_EPD_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(wait_busy(CONFIG_KIBAN_EPD_BUSY_TIMEOUT_MS, "reset", NULL, NULL), TAG, "reset busy");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static esp_err_t bus_up(void)
{
    if (s.bus_up) return ESP_OK;
    // RST idle high (push-pull), CS idle high, DC, BUSY input with pull-down [vendor .c:278-309].
    ESP_RETURN_ON_ERROR(gpio_set_level(KIBAN_EPD_PIN_RST, 1), TAG, "rst");
    ESP_RETURN_ON_ERROR(gpio_set_level(KIBAN_EPD_PIN_CS, 1), TAG, "cs");
    const gpio_config_t out = {
        .pin_bit_mask = (1ULL << KIBAN_EPD_PIN_RST) | (1ULL << KIBAN_EPD_PIN_CS) | (1ULL << KIBAN_EPD_PIN_DC),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "out pins");
    const gpio_config_t in = {.pin_bit_mask = 1ULL << KIBAN_EPD_PIN_BUSY, .mode = GPIO_MODE_INPUT,
                              .pull_down_en = GPIO_PULLDOWN_ENABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&in), TAG, "busy pin");

    if (!s.bounce) {
        s.bounce = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        ESP_RETURN_ON_FALSE(s.bounce, ESP_ERR_NO_MEM, TAG, "bounce buffer");
    }
    // SPI3, mode 0, no MISO, 10 MHz [vendor metalio_epd.cc:21-41, config.h:42-49]. CS is driven by
    // hand so that one command and its multi-chunk data share a single CS frame.
    const spi_bus_config_t bus = {
        .mosi_io_num = KIBAN_EPD_PIN_MOSI, .miso_io_num = -1, .sclk_io_num = KIBAN_EPD_PIN_SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = CHUNK_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");
    const spi_device_interface_config_t devcfg = {
        .mode = 0, .clock_speed_hz = CONFIG_KIBAN_EPD_SPI_HZ, .spics_io_num = -1, .queue_size = 1,
    };
    esp_err_t err = spi_bus_add_device(SPI3_HOST, &devcfg, &s.dev);
    if (err != ESP_OK) {
        spi_bus_free(SPI3_HOST);
        ESP_LOGE(TAG, "spi device: %s", esp_err_to_name(err));
        return err;
    }
    s.bus_up = true;
    return ESP_OK;
}

esp_err_t kiban_epd_init(void)
{
    s.ready = false;
    s.have_base = false;
    s.lut = LUT_NONE;
    ESP_RETURN_ON_ERROR(bus_up(), TAG, "bus");
    ESP_RETURN_ON_ERROR(hw_reset(), TAG, "reset");

    // [vendor esp_lcd_panel_ssd1677.c:401-438]
    ESP_RETURN_ON_ERROR(cmd(CMD_SWRESET, NULL, 0), TAG, "swreset");
    vTaskDelay(pdMS_TO_TICKS(10));
    // FreeInk also waits for BUSY after the 10 ms (Ssd1677Driver.cpp:384-389).
    ESP_RETURN_ON_ERROR(wait_busy(CONFIG_KIBAN_EPD_BUSY_TIMEOUT_MS, "swreset", NULL, NULL), TAG, "swreset busy");
    ESP_RETURN_ON_ERROR(CMD1(CMD_TEMP_SENSOR, 0x80), TAG, "temp sensor");   // internal sensor
    // Display option, byte 6 = 0x40 (vendor comment: ping-pong option; always sent, the #if is commented out).
    ESP_RETURN_ON_ERROR(cmd(CMD_DISPLAY_OPTION, (const uint8_t[]){0, 0, 0, 0, 0, 0x40, 0, 0, 0, 0}, 10), TAG, "0x37");
    ESP_RETURN_ON_ERROR(cmd(CMD_BOOSTER, (const uint8_t[]){0xAE, 0xC7, 0xC3, 0xC0, 0x80}, 5), TAG, "booster");
    // 480 gate lines (0x1DF), scan byte 0x02.
    ESP_RETURN_ON_ERROR(cmd(CMD_DRIVER_OUTPUT, (const uint8_t[]){(KIBAN_EPD_HEIGHT - 1) & 0xFF,
                                                                 (KIBAN_EPD_HEIGHT - 1) >> 8, 0x02}, 3),
                        TAG, "driver output");
    ESP_RETURN_ON_ERROR(CMD1(CMD_BORDER, BORDER_FULL), TAG, "border");
    ESP_RETURN_ON_ERROR(load_lut(LUT_FULL, LUT_IS_FULL), TAG, "full lut");
    ESP_RETURN_ON_ERROR(set_ram_area_full(), TAG, "ram area");
    s.analog_on = false;
    s.ready = true;
    ESP_LOGI(TAG, "SSD1677 ready %dx%d", KIBAN_EPD_WIDTH, KIBAN_EPD_HEIGHT);
    return ESP_OK;
}

esp_err_t kiban_epd_sleep(void)
{
    ESP_RETURN_ON_FALSE(s.bus_up, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    if (s.analog_on) {
        // Power down first (FreeInk powerOffController before 0x10, Ssd1677Driver.cpp:962-972);
        // 0x83 is the vendor power-off sequence [commands.h:86].
        ESP_RETURN_ON_ERROR(CMD1(CMD_UPDATE_CTRL2, CTRL2_POWER_OFF), TAG, "ctrl2");
        ESP_RETURN_ON_ERROR(cmd(CMD_ACTIVATE, NULL, 0), TAG, "activate");
        ESP_RETURN_ON_ERROR(wait_busy(CONFIG_KIBAN_EPD_BUSY_TIMEOUT_MS, "power off", NULL, NULL), TAG, "power off");
        s.analog_on = false;
    }
    // Deep sleep mode 1 [vendor commands.h:63-67, esp_lcd_panel_ssd1677.c:577-587; never called by the vendor UI].
    ESP_RETURN_ON_ERROR(CMD1(CMD_DEEP_SLEEP, 0x01), TAG, "deep sleep");
    s.ready = false;
    s.have_base = false;
    s.lut = LUT_NONE;
    ESP_LOGI(TAG, "deep sleep");
    return ESP_OK;
}

esp_err_t kiban_epd_deinit(void)
{
    if (s.bus_up) {
        spi_bus_remove_device(s.dev);
        spi_bus_free(SPI3_HOST);
        s.dev = NULL;
        s.bus_up = false;
    }
    gpio_reset_pin(KIBAN_EPD_PIN_CS);
    gpio_reset_pin(KIBAN_EPD_PIN_DC);
    gpio_reset_pin(KIBAN_EPD_PIN_RST);
    gpio_reset_pin(KIBAN_EPD_PIN_BUSY);
    heap_caps_free(s.bounce);
    s.bounce = NULL;
    s.ready = false;
    s.have_base = false;
    s.lut = LUT_NONE;
    return ESP_OK;
}

kiban_epd_stats_t kiban_epd_last_stats(void) { return s.stats; }
