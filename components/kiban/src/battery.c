#include "kiban/battery.h"

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "kiban.battery";
static bool s_temp_blocked;

static void temperature_gate(int16_t t, bool *blocked)
{
    if (!kiban_charger_policy_ok()) return;   // charging is not ours to enable
    const bool too_cold = t < KIBAN_CHARGE_TEMP_MIN_C_X10, too_hot = t > KIBAN_CHARGE_TEMP_MAX_C_X10;
    if (!s_temp_blocked && (too_cold || too_hot)) {
        if (kiban_charger_set_enabled(false) == ESP_OK) {
            s_temp_blocked = true;
            ESP_LOGW(TAG, "charging stopped: temperature %d.%d C", t / 10, t % 10);
        }
    } else if (s_temp_blocked && t >= KIBAN_CHARGE_TEMP_MIN_C_X10 + KIBAN_CHARGE_TEMP_HYST_C_X10 &&
               t <= KIBAN_CHARGE_TEMP_MAX_C_X10 - KIBAN_CHARGE_TEMP_HYST_C_X10) {
        if (kiban_charger_set_enabled(true) == ESP_OK) {
            s_temp_blocked = false;
            ESP_LOGI(TAG, "charging resumed: temperature %d.%d C", t / 10, t % 10);
        }
    }
    *blocked = s_temp_blocked;
}

esp_err_t kiban_battery_poll(kiban_soc_t *soc, int64_t now_us, kiban_battery_t *o)
{
    ESP_RETURN_ON_ERROR(kiban_gauge_read_basic(&o->gauge), TAG, "gauge");
    ESP_RETURN_ON_ERROR(kiban_charger_status(&o->charger), TAG, "charger");
    o->charger_ok = kiban_charger_policy_ok();
    temperature_gate(o->gauge.temp_c_x10, &o->temp_blocked);

    const bool charging = o->charger.chrg_stat >= 1;
    const bool full = !charging && o->charger.en_chg && o->gauge.avg_current_ma >= -5 &&
                      o->gauge.voltage_mv + 80 >= o->charger.vreg_mv;
    o->state = charging ? KIBAN_BAT_CHARGING : full ? KIBAN_BAT_FULL : KIBAN_BAT_DISCHARGING;

    const kiban_soc_sample_t in = {
        .mv = o->gauge.voltage_mv, .ma_now = o->gauge.current_ma, .ma_avg = o->gauge.avg_current_ma,
        .charging = charging, .terminated = full, .vreg_mv = (uint16_t)o->charger.vreg_mv, .now_us = now_us,
    };
    o->soc_pct = kiban_soc_update(soc, &in);
    o->display_pct = kiban_soc_display(soc, now_us, charging);
    o->critical = soc->critical;
    return ESP_OK;
}
