// State-of-charge estimator independent of the fuel gauge's (unconfigurable) model: coulomb counting
// on the gauge's AverageCurrent() with a known capacity, anchored to "charge terminated" (= 100 %) and,
// after a rest period, to an open-circuit-voltage table. Pure computation; no hardware access, so it
// can be unit-tested on the host.
//
// Time base: `now_us` must be monotonic within one boot (esp_timer). Gaps longer than `max_gap_us`
// (deep sleep, power-off, a reboot) are never integrated: the estimate is re-anchored to the voltage
// table instead, because the battery has been resting during the gap. Persisting soc_pct across a true
// power-off needs storage chosen by the product (open).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float capacity_mah;     // usable capacity; ~1000 mAh for this device [community], unverified
    int64_t max_gap_us;     // samples further apart than this are treated as a gap (default 120 s)
    float soc_pct;          // current estimate 0..100
    bool valid;             // false until the first sample (or a restore)
    int64_t last_us;
    int64_t rest_since_us;  // start of the current low-current period, or -1
    uint8_t shown_pct;      // display value with hysteresis
} kiban_soc_t;

typedef struct {
    uint16_t mv;            // terminal voltage
    int16_t ma;             // + charging, - discharging (gauge AverageCurrent(), 0x14)
    bool charge_terminated; // charger reports done/not charging while not discharging
    int64_t now_us;
} kiban_soc_sample_t;

void kiban_soc_init(kiban_soc_t *s, float capacity_mah);
// Start from a saved value (e.g. stored before power-off) instead of the voltage table.
void kiban_soc_restore(kiban_soc_t *s, float soc_pct, int64_t now_us);
// Feed one sample (any interval; 1-60 s typical). Returns the updated estimate 0..100.
float kiban_soc_update(kiban_soc_t *s, const kiban_soc_sample_t *in);
// Integer for display: moves at most 1 % per call and only in the direction of the estimate.
uint8_t kiban_soc_display(kiban_soc_t *s);
// Generic 4.2 V Li-ion open-circuit-voltage curve (to be replaced by a measured one).
float kiban_soc_from_ocv(float ocv_mv);

#ifdef __cplusplus
}
#endif
