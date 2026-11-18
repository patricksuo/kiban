// TCA9555 16-bit I2C IO expander at 0x20 (Metalio E-Ink 4).
// Pin numbers 0..15: 0..7 = P0.0..P0.7, 8..15 = P1.0..P1.7.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KIBAN_IOEXP_ADDR 0x20

// Register pairs (low byte = port 0, high byte = port 1).
#define KIBAN_IOEXP_REG_INPUT    0x00
#define KIBAN_IOEXP_REG_OUTPUT   0x02
#define KIBAN_IOEXP_REG_POLARITY 0x04
#define KIBAN_IOEXP_REG_CONFIG   0x06  // 1 = input, 0 = output

// Pin map [source] vendor firmware IOExpander.hpp:46-57, FreeInk MetalioEInk4Board.h:12-20.
#define KIBAN_IOEXP_USB_MUX_SEL   0   // high = ESP USB (flash/debug), low = camera
#define KIBAN_IOEXP_PA_SWITCH     1   // 0 = ESP audio, 1 = 4G voice
#define KIBAN_IOEXP_MODEM_RST     3
#define KIBAN_IOEXP_PA_EN         4   // amplifier enable
#define KIBAN_IOEXP_PANEL_SD_PWR  5   // panel socket + SD card rail
#define KIBAN_IOEXP_MAIN_PWR      6   // main peripheral rail (BT audio module, modem)
#define KIBAN_IOEXP_VOL_DOWN      7   // input, active low
#define KIBAN_IOEXP_VOL_UP        8   // input, active low
#define KIBAN_IOEXP_TOUCH_RST     9   // active low
#define KIBAN_IOEXP_PWR_KEY_PULSE 11  // idle high; pulse train = power-off request to the PMIC
#define KIBAN_IOEXP_ACCEL_INT     12  // input

// Probe the expander, retrying (with I2C bus recovery) for up to wait_ms.
// Some boards do not answer on I2C until ~2 s after power-up [community] FreeInk 29ae30b.
esp_err_t kiban_ioexp_init(i2c_master_bus_handle_t bus, uint32_t wait_ms);
esp_err_t kiban_ioexp_read16(uint8_t reg, uint16_t *out);
esp_err_t kiban_ioexp_write16(uint8_t reg, uint16_t value);
// Read-modify-write of the output latch; serialised with an internal mutex.
esp_err_t kiban_ioexp_set(uint8_t pin, bool high);
esp_err_t kiban_ioexp_get_input(uint8_t pin, bool *high);

#ifdef __cplusplus
}
#endif
