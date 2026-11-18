#include "kiban/battery_soc.h"

#include <math.h>

// Generic 4.2 V Li-ion OCV curve [inference: typical datasheet shape, not measured on this cell].
static const struct { float mv, pct; } OCV[] = {
    {3300, 0}, {3500, 3}, {3600, 8}, {3680, 15}, {3730, 25}, {3770, 35}, {3800, 45},
    {3840, 55}, {3890, 65}, {3950, 75}, {4020, 85}, {4080, 92}, {4150, 98}, {4200, 100},
};
#define N_OCV (sizeof OCV / sizeof OCV[0])

#define R_INTERNAL_OHM   0.15f    // [inference] ocv = v - i * R
#define REST_MA          10
#define REST_US          (30LL * 60 * 1000000)
#define REST_TAU_S       600.0    // once rested, pull towards OCV with this time constant
#define OCV_BAND_PCT     15.0
#define FULL_MARGIN_MV   80       // 100 % anchor needs v >= vreg - margin
#define EMPTY_MV         3300
#define CRITICAL_MV      3350
#define EMPTY_SAMPLES    3
#define DISPLAY_DEADBAND 0.6
#define DISPLAY_STEP_US  (20LL * 1000000)

float kiban_soc_from_ocv(float mv)
{
    if (!(mv > OCV[0].mv)) return 0;   // also catches NaN
    if (mv >= OCV[N_OCV - 1].mv) return 100;
    for (unsigned i = 1; i < N_OCV; i++)
        if (mv < OCV[i].mv)
            return OCV[i - 1].pct + (OCV[i].pct - OCV[i - 1].pct) * (mv - OCV[i - 1].mv) / (OCV[i].mv - OCV[i - 1].mv);
    return 100;
}

static double clampd(double v, double lo, double hi) { return !(v >= lo) ? lo : v > hi ? hi : v; }

void kiban_soc_init(kiban_soc_t *s, float capacity_mah)
{
    *s = (kiban_soc_t){
        .capacity_mah = capacity_mah > 0 ? capacity_mah : KIBAN_BATTERY_CAPACITY_MAH,
        .max_gap_us = 120LL * 1000000,
        .sleep_ma = 3.0f,
        .rest_since_us = -1,
        .pending_gap_s = -2,   // -2: none declared
    };
}

void kiban_soc_restore(kiban_soc_t *s, float soc_pct, int64_t now_us)
{
    if (!(soc_pct >= 0 && soc_pct <= 100)) return;   // rejects NaN / out of range
    s->soc_pct = soc_pct;
    s->shown_pct = (uint8_t)lround(soc_pct);
    s->shown_us = now_us;
    s->valid = true;
    s->last_us = now_us;
    s->rest_since_us = -1;
}

void kiban_soc_resume(kiban_soc_t *s, float gap_s) { s->pending_gap_s = gap_s >= 0 ? gap_s : -1; }

float kiban_soc_update(kiban_soc_t *s, const kiban_soc_sample_t *in)
{
    const float comp_mv = (float)in->mv - (float)in->ma_now * R_INTERNAL_OHM;
    const double ocv_pct = kiban_soc_from_ocv(comp_mv);

    // Low-voltage tracking first: it must work even on the very first sample.
    s->critical = comp_mv < CRITICAL_MV && in->ma_avg <= 0;
    s->low_count = (comp_mv <= EMPTY_MV && in->ma_avg <= 0) ? (uint8_t)(s->low_count < 255 ? s->low_count + 1 : 255) : 0;

    if (!s->valid) {
        kiban_soc_restore(s, (float)ocv_pct, in->now_us);
        s->pending_gap_s = -2;
        return (float)s->soc_pct;
    }
    const int64_t dt_us = in->now_us - s->last_us;
    s->last_us = in->now_us;
    float gap_s = s->pending_gap_s;
    if (gap_s < -1.5f && (dt_us < 0 || dt_us > s->max_gap_us)) gap_s = dt_us > 0 ? (float)dt_us / 1e6f : -1;
    s->pending_gap_s = -2;

    if (gap_s > -1.5f) {
        // Gap: subtract the assumed sleep draw, then blend towards OCV. The cell rested during the gap, so
        // the voltage is informative unless the device is already drawing (or charging) heavily.
        if (gap_s > 0) s->soc_pct -= (double)s->sleep_ma * gap_s / 3600.0 / s->capacity_mah * 100.0;
        const double w = (in->ma_now > -REST_MA * 2 && in->ma_now < REST_MA * 2) ? 0.5 : 0.2;
        if (!in->charging) s->soc_pct += (ocv_pct - s->soc_pct) * w;
        s->rest_since_us = -1;
    } else {
        s->soc_pct += (double)in->ma_avg * ((double)dt_us / 3.6e9) / s->capacity_mah * 100.0;
        if (in->ma_avg > -REST_MA && in->ma_avg < REST_MA) {
            if (s->rest_since_us < 0) s->rest_since_us = in->now_us;
            if (in->now_us - s->rest_since_us >= REST_US) {
                const double k = clampd((double)dt_us / 1e6 / REST_TAU_S, 0, 1);
                s->soc_pct += (ocv_pct - s->soc_pct) * k;
            }
        } else {
            s->rest_since_us = -1;
        }
    }

    // Anchors and bounds.
    if (in->terminated && in->vreg_mv && in->mv + FULL_MARGIN_MV >= in->vreg_mv && in->ma_avg >= -5) {
        s->soc_pct = 100;
    } else if (in->charging) {
        if (s->soc_pct > 99) s->soc_pct = 99;
    } else {
        s->soc_pct = clampd(s->soc_pct, ocv_pct - OCV_BAND_PCT, ocv_pct + OCV_BAND_PCT);
    }
    if (s->low_count >= EMPTY_SAMPLES) s->soc_pct = 0;
    s->soc_pct = clampd(s->soc_pct, 0, 100);
    return (float)s->soc_pct;
}

uint8_t kiban_soc_display(kiban_soc_t *s, int64_t now_us, bool charging)
{
    if (now_us - s->shown_us < DISPLAY_STEP_US) return s->shown_pct;
    const double diff = s->soc_pct - (double)s->shown_pct;
    // Rising is allowed while charging, or to reach a 100 % anchor (charge terminated).
    const bool may_rise = charging || s->soc_pct >= 100;
    if (diff >= DISPLAY_DEADBAND && may_rise && s->shown_pct < 100) {
        s->shown_pct++;
        s->shown_us = now_us;
    } else if (diff <= -DISPLAY_DEADBAND && !charging && s->shown_pct > 0) {
        s->shown_pct--;
        s->shown_us = now_us;
    }
    return s->shown_pct;
}
