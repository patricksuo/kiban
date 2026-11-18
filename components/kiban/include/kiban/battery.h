// One entry point for products: read gauge + charger, decide the charging state, apply the
// temperature gate, feed the SOC estimator. "External power present" cannot be read directly
// (the charger's D+/D- detection is off, VBUS_STAT reads 0): it is inferred from the charger state.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "kiban/battery_soc.h"
#include "kiban/charger.h"
#include "kiban/sensors.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KIBAN_BAT_DISCHARGING = 0,   // on battery, or external power with charging stopped
    KIBAN_BAT_CHARGING,          // charger active (pre-charge / CC / CV / top-off) and current flowing in
    KIBAN_BAT_FULL,              // charge terminated, external power present
} kiban_bat_state_t;

typedef struct {
    kiban_gauge_basic_t gauge;
    kiban_charger_status_t charger;
    kiban_bat_state_t state;
    float soc_pct;
    uint8_t display_pct;
    bool critical;        // power off now (IR-compensated voltage below ~3.35 V)
    bool temp_blocked;    // charging stopped by the temperature gate
    bool charger_ok;      // charge policy applied and verified
} kiban_battery_t;

// Charging is allowed between these temperatures (gauge temperature; its source is [to-verify]).
#define KIBAN_CHARGE_TEMP_MIN_C_X10 0
#define KIBAN_CHARGE_TEMP_MAX_C_X10 450
#define KIBAN_CHARGE_TEMP_HYST_C_X10 20

esp_err_t kiban_battery_poll(kiban_soc_t *soc, int64_t now_us, kiban_battery_t *out);

#ifdef __cplusplus
}
#endif
