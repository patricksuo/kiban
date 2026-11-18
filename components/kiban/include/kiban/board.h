// Board bring-up for the Metalio E-Ink 4.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// ESP32-S3 pins [source] vendor firmware config.h, FreeInk MetalioEInk4Board.h.
#define KIBAN_PIN_I2C_SDA     41
#define KIBAN_PIN_I2C_SCL     42
#define KIBAN_PIN_BOOT_KEY    0
#define KIBAN_PIN_POWER_KEY   3
#define KIBAN_PIN_TOUCH_INT   1
#define KIBAN_PIN_IOEXP_INT   2
#define KIBAN_PIN_MOTOR       44
#define KIBAN_PIN_SD_DAT3     46   // input-only; must read high before SD init

typedef enum {
    KIBAN_RAIL_MAIN = 0,      // P0.6: BT audio module, modem
    KIBAN_RAIL_PANEL_SD,      // P0.5: e-paper panel socket + SD card
} kiban_rail_t;

typedef struct {
    uint32_t i2c_hz;           // default 400 kHz
    uint32_t expander_wait_ms; // how long to retry the expander at boot; default 3000
    uint32_t charge_vreg_mv;   // charger termination voltage enforced at init; capped at
                               // KIBAN_CHARGER_VREG_CAP_MV (4200). 0 = leave the charger untouched
} kiban_board_cfg_t;

#define KIBAN_BOARD_CFG_DEFAULT() { .i2c_hz = 400000, .expander_wait_ms = 3000, .charge_vreg_mv = 4200 }

// Bring-up, in this order:
//  1. I2C master bus on SDA41/SCL42.
//  2. TCA9555 with retries; output latch written BEFORE direction so no pin glitches:
//     USB mux -> ESP, PA switch -> ESP, amp OFF, main rail ON, power-off pulse line idle HIGH,
//     touch held in reset.
//  3. Panel/SD rail ON, 10 ms, release touch reset, 120 ms settle.
//  4. Motor GPIO low.
//  5. Charger VREG set to cfg->charge_vreg_mv (default 4200 mV) and verified.
// Mirrors the vendor boot levels; no power-off pulses.
esp_err_t kiban_board_init(const kiban_board_cfg_t *cfg);

i2c_master_bus_handle_t kiban_board_i2c(void);
esp_err_t kiban_board_rail_set(kiban_rail_t rail, bool on);

#ifdef __cplusplus
}
#endif
