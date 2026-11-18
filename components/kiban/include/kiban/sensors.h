// Read paths for the fuel gauge (BQ27220), charger status (CX25601N, read-only), RTC (PCF8563)
// and accelerometer (SC7A20H). All require kiban_board_init().
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KIBAN_GAUGE_ADDR   0x55
#define KIBAN_CHARGER_ADDR 0x6B
#define KIBAN_RTC_ADDR     0x51
#define KIBAN_IMU_ADDR     0x19

typedef struct {
    uint16_t voltage_mv;
    int16_t current_ma;         // + charging, - discharging
    int16_t avg_current_ma;
    int16_t temp_c_x10;
    uint16_t soc_pct;           // gauge's own StateOfCharge
    uint16_t remaining_mah;
    uint16_t full_mah;
    uint16_t design_mah;
    uint16_t cycle_count;
    uint16_t soh_pct;
    uint16_t charge_voltage_mv; // gauge's configured ChargingVoltage (NOT the charger's VREG)
    uint16_t charge_current_ma;
    uint16_t battery_status;
} kiban_gauge_t;

esp_err_t kiban_gauge_read(kiban_gauge_t *out);

typedef struct {
    uint8_t regs[0x40];         // raw dump 0x00..0x3F
    uint32_t vreg_mv;           // charge termination voltage currently programmed
    uint32_t ichg_ma;           // fast-charge current
    uint32_t iindpm_ma;         // input current limit
    uint8_t vbus_stat;          // 0 none, 1 SDP, 2 CDP, 3 DCP, 4 unknown, 5 non-standard, 7 OTG
    uint8_t chrg_stat;          // 0 not charging/terminated, 1 pre/CC, 2 CV, 3 top-off
} kiban_charger_t;

// READ-ONLY. Never writes the charger (no unlock sequence, no register writes).
esp_err_t kiban_charger_read(kiban_charger_t *out);

typedef struct {
    int year, month, day, weekday, hour, minute, second;
    bool integrity_lost;        // PCF8563 VL bit: clock was not guaranteed (power loss)
} kiban_rtc_time_t;

esp_err_t kiban_rtc_get(kiban_rtc_time_t *out);

typedef struct { int16_t x_mg, y_mg, z_mg; } kiban_imu_sample_t;

esp_err_t kiban_imu_init(uint8_t *who_am_i);   // 100 Hz, XYZ, +/-2 g high-resolution
esp_err_t kiban_imu_read(kiban_imu_sample_t *out);

#ifdef __cplusplus
}
#endif
