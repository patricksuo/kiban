// Fuel gauge (BQ27220, read-only), RTC (PCF8563) and accelerometer (SC7A20H).
// Charger: see charger.h. All require kiban_board_init().
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KIBAN_GAUGE_ADDR 0x55
#define KIBAN_RTC_ADDR   0x51
#define KIBAN_IMU_ADDR   0x19

// The BQ27220 is sealed with non-default keys on this device [measured]: its capacity model stays at
// the 3000 mAh default, so soc_pct / remaining / full are NOT meaningful. Voltage, current and
// temperature are. Use battery_soc.h for a state of charge.
typedef struct {
    uint16_t voltage_mv;
    int16_t current_ma;         // + charging, - discharging
    int16_t avg_current_ma;
    int16_t temp_c_x10;         // source (internal sensor or NTC) not yet confirmed [to-verify]
    uint16_t soc_pct, remaining_mah, full_mah, design_mah;   // gauge model (3000 mAh default), diagnostic only
    uint16_t cycle_count, soh_pct, charge_voltage_mv, charge_current_ma;
    uint16_t battery_status;
    uint16_t operation_status;  // 0x3A: SEC[1:0] bits 2:1, CFGUPDATE bit 10
} kiban_gauge_t;

typedef struct {
    uint16_t voltage_mv;
    int16_t current_ma, avg_current_ma, temp_c_x10;
} kiban_gauge_basic_t;

esp_err_t kiban_gauge_read(kiban_gauge_t *out);               // all fields (14 transactions)
esp_err_t kiban_gauge_read_basic(kiban_gauge_basic_t *out);   // V, I, avg I, T (4 transactions)

typedef struct {
    int year, month, day, weekday, hour, minute, second;
    bool integrity_lost;        // PCF8563 VL bit: clock was not guaranteed (power loss)
} kiban_rtc_time_t;

esp_err_t kiban_rtc_get(kiban_rtc_time_t *out);
// Countdown timer interrupt after `seconds` (1 Hz source). The INT line reaches the power-switch chip:
// armed before kiban_power_off(), it powers the board back on [measured] research/003 P7.
esp_err_t kiban_rtc_timer_irq(uint8_t seconds);
esp_err_t kiban_rtc_timer_off(void);

typedef struct { int16_t x_mg, y_mg, z_mg; } kiban_imu_sample_t;

esp_err_t kiban_imu_init(uint8_t *who_am_i);   // 100 Hz, XYZ, +/-2 g high-resolution
esp_err_t kiban_imu_read(kiban_imu_sample_t *out);

#ifdef __cplusplus
}
#endif
