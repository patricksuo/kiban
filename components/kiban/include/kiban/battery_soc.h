// State-of-charge estimator independent of the fuel gauge's (sealed, 3000 mAh-default) model.
//  - Coulomb counting on the gauge's AverageCurrent() with a known capacity.
//  - Anchors: 100 % at charge termination; 0 % after several consecutive low IR-compensated samples;
//    after a rest period, a slow pull towards an open-circuit-voltage (OCV) table.
//  - While not charging the estimate is kept within +/-15 % of the OCV estimate.
//  - Gaps (deep sleep, power-off, reboot) are never integrated with the waking current: call
//    kiban_soc_resume() with the gap length (e.g. from the RTC) at boot; an undeclared gap
//    (dt > max_gap_us or time going backwards) is handled the same way with gap = dt or unknown.
// Pure computation; host-testable (tests/host/test_battery_soc.c).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Cell capacity: ~1000 mAh [community] (comparisontabl.es), unverified on the device.
#define KIBAN_BATTERY_CAPACITY_MAH 1000

typedef struct {
    // configuration
    float capacity_mah;     // > 0
    int64_t max_gap_us;     // samples further apart are a gap (default 120 s)
    float sleep_ma;         // average draw during a gap (default 11.5 mA: deep sleep with rails on, measured research/003)
    // state
    double soc_pct;         // estimate 0..100
    bool valid;
    int64_t last_us;
    int64_t rest_since_us;  // start of the current low-current period, or -1
    float pending_gap_s;    // declared by kiban_soc_resume(); < 0 = none
    uint8_t low_count;      // consecutive samples below the empty threshold
    bool critical;          // IR-compensated voltage below the critical threshold
    uint8_t shown_pct;      // display value
    int64_t shown_us;       // last display step
} kiban_soc_t;

typedef struct {
    uint16_t mv;            // terminal voltage
    int16_t ma_now;         // instantaneous current (Current(), 0x0C) for IR compensation
    int16_t ma_avg;         // average current (AverageCurrent(), 0x14) for integration; + = charging
    bool charging;          // charger in pre-charge / CC / CV / top-off
    bool terminated;        // external power present and charge terminated
    uint16_t vreg_mv;       // charger termination voltage (for the 100 % anchor)
    int64_t now_us;         // monotonic within one boot (esp_timer)
} kiban_soc_sample_t;

void kiban_soc_init(kiban_soc_t *s, float capacity_mah);
// Start from a saved estimate (e.g. kept in RTC memory or flash). Invalid values are ignored.
void kiban_soc_restore(kiban_soc_t *s, float soc_pct, int64_t now_us);
// Declare a gap of gap_s seconds (sleep / power-off) before the next update; < 0 = unknown length.
void kiban_soc_resume(kiban_soc_t *s, float gap_s);
float kiban_soc_update(kiban_soc_t *s, const kiban_soc_sample_t *in);
// Display value: deadband 0.6 %, at most one step per 20 s, never rises while discharging and never
// falls while charging.
uint8_t kiban_soc_display(kiban_soc_t *s, int64_t now_us, bool charging);
// Generic 4.2 V Li-ion OCV curve [inference], to be replaced by a measured one.
float kiban_soc_from_ocv(float ocv_mv);

#ifdef __cplusplus
}
#endif
