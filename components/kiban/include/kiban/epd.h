// SSD1677 e-paper panel (GDEM0397T81, 800x480) on the Metalio E-Ink 4.
//
// Sequences and waveforms are ported from the vendor firmware (MIT,
// https://github.com/CloudZao/Metalio-E-INK4 @ d12f481) and FreeInk SDK (MIT,
// https://github.com/freeink/freeink-sdk @ e637713); see src/epd.c for per-step citations.
//
// Not thread-safe: call all functions from one task. No tasks, timers or ISRs are created.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ESP32-S3 pins. DC/RST follow the vendor's runnable config.h (DC13/RST18), which FreeInk also uses;
// the vendor pin spreadsheet has them swapped.
#define KIBAN_EPD_PIN_SCLK  14
#define KIBAN_EPD_PIN_MOSI  8
#define KIBAN_EPD_PIN_CS    45
#define KIBAN_EPD_PIN_DC    13
#define KIBAN_EPD_PIN_RST   18
#define KIBAN_EPD_PIN_BUSY  9   // high = busy

// Frame geometry, in the controller's native orientation (landscape).
// 1bpp frame: row-major, 100 bytes per row, MSB = leftmost pixel, bit 1 = white, 0 = black.
// 2bpp frame: row-major, 200 bytes per row, 4 pixels per byte, first pixel in bits 7..6;
//             value 0 = black, 1 = dark gray, 2 = light gray, 3 = white.
#define KIBAN_EPD_WIDTH       800
#define KIBAN_EPD_HEIGHT      480
#define KIBAN_EPD_FB_BYTES    (KIBAN_EPD_WIDTH * KIBAN_EPD_HEIGHT / 8)   // 48000
#define KIBAN_EPD_FB2_BYTES   (KIBAN_EPD_WIDTH * KIBAN_EPD_HEIGHT / 4)   // 96000

typedef enum {
    // Vendor "normal" full refresh: MCU-loaded waveform (vendor fast-update LUT), 0x22=0xC7.
    // Writes the frame to both RAM planes.
    KIBAN_EPD_MODE_FULL = 0,
    // Vendor "fast full": OTP waveform selected with a forced temperature (0x1A=0x6A), 0x22=0xD7.
    // Present but unused in the vendor firmware.
    KIBAN_EPD_MODE_FAST,
    // Differential refresh with the vendor partial LUT, 0x22=0xCF. Writes the new frame to 0x24;
    // 0x26 must hold the frame currently on the glass, which every kiban_epd_show*() leaves there.
    // Returns ESP_ERR_INVALID_STATE if no refresh has run since kiban_epd_init().
    // Repeated partials accumulate ghosting; when to interleave a FULL is the caller's policy
    // (the vendor UI forces one after 15 partials).
    KIBAN_EPD_MODE_PARTIAL,
    // Full refresh with the panel's own OTP waveform and the internal temperature sensor,
    // 0x22=0xF7 (FreeInk's Metalio full refresh). Not used by the vendor firmware.
    KIBAN_EPD_MODE_FULL_OTP,
} kiban_epd_mode_t;

// Timing of the most recent refresh, for measurement.
typedef struct {
    uint32_t transfer_ms;  // SPI time for the RAM writes before activation
    uint32_t busy_ms;      // MASTER_ACTIVATION -> BUSY low; resolution is one RTOS tick
    bool busy_seen;        // BUSY was observed high after activation (false: no refresh evidence)
} kiban_epd_stats_t;

// Requires the panel rail on (kiban_board_init() does that). Sets up SPI3 at 10 MHz with a 4 KB
// internal DMA bounce buffer, hardware-resets the controller and runs the vendor init sequence.
// The glass is not touched; the first refresh after init cannot be PARTIAL.
// Calling it again re-initialises the controller (use after kiban_epd_sleep() or a timeout).
esp_err_t kiban_epd_init(void);

// Releases SPI and GPIOs. Does not put the controller to sleep or switch any rail.
esp_err_t kiban_epd_deinit(void);

// Shows a 1bpp frame (caller-owned, any memory, KIBAN_EPD_FB_BYTES long) and blocks until the
// refresh completes. timeout_ms bounds every BUSY wait; 0 selects CONFIG_KIBAN_EPD_BUSY_TIMEOUT_MS.
// On ESP_ERR_TIMEOUT the driver needs kiban_epd_init() again; the rail is left as it is.
esp_err_t kiban_epd_show(const uint8_t *fb, kiban_epd_mode_t mode, uint32_t timeout_ms);

// Shows a 2bpp 4-level frame (KIBAN_EPD_FB2_BYTES) using the vendor overlay path: a FULL refresh
// of the 1-bit base (every non-white pixel black), then the vendor 4-gray LUT (0x22=0xCC) lightens
// dark-gray / light-gray pixels. Afterwards both RAM planes hold the 1-bit base, so a following
// PARTIAL diffs against the base (the gray pixels are not cleaned by it).
esp_err_t kiban_epd_show_gray(const uint8_t *fb2, uint32_t timeout_ms);

// Powers the analog block down if it is on and enters controller deep sleep (0x10=0x01).
// The image stays on the glass. kiban_epd_init() is required before the next refresh.
esp_err_t kiban_epd_sleep(void);

// Stats of the last refresh (for show_gray: the gray pass; the base pass is logged).
kiban_epd_stats_t kiban_epd_last_stats(void);

#ifdef __cplusplus
}
#endif
