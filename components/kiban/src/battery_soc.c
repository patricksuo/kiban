#include "kiban/battery_soc.h"

#include <math.h>

// Generic 4.2 V Li-ion OCV curve [inference: typical datasheet shape, not measured on this cell].
static const struct { float mv, pct; } OCV[] = {
    {3300, 0}, {3500, 3}, {3600, 8}, {3680, 15}, {3730, 25}, {3770, 35}, {3800, 45},
    {3840, 55}, {3890, 65}, {3950, 75}, {4020, 85}, {4080, 92}, {4150, 98}, {4200, 100},
};
#define N_OCV (sizeof OCV / sizeof OCV[0])

#define R_INTERNAL_OHM   0.15f    // terminal-voltage compensation: ocv = v - i*R
#define REST_MA          10       // |I| below this counts as rest
#define REST_US          (30LL * 60 * 1000000)
#define REST_BLEND       0.2f     // per update once rested
#define FULL_MV          4150

float kiban_soc_from_ocv(float mv)
{
    if (mv <= OCV[0].mv) return 0;
    if (mv >= OCV[N_OCV - 1].mv) return 100;
    for (unsigned i = 1; i < N_OCV; i++)
        if (mv < OCV[i].mv)
            return OCV[i - 1].pct + (OCV[i].pct - OCV[i - 1].pct) * (mv - OCV[i - 1].mv) / (OCV[i].mv - OCV[i - 1].mv);
    return 100;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void kiban_soc_init(kiban_soc_t *s, float capacity_mah)
{
    *s = (kiban_soc_t){.capacity_mah = capacity_mah, .max_gap_us = 120LL * 1000000, .rest_since_us = -1};
}

void kiban_soc_restore(kiban_soc_t *s, float soc_pct, int64_t now_us)
{
    s->soc_pct = clampf(soc_pct, 0, 100);
    s->shown_pct = (uint8_t)lroundf(s->soc_pct);
    s->valid = true;
    s->last_us = now_us;
    s->rest_since_us = -1;
}

float kiban_soc_update(kiban_soc_t *s, const kiban_soc_sample_t *in)
{
    const float ocv_pct = kiban_soc_from_ocv((float)in->mv - (float)in->ma * R_INTERNAL_OHM);
    if (!s->valid) {
        kiban_soc_restore(s, ocv_pct, in->now_us);
        return s->soc_pct;
    }
    const int64_t dt_us = in->now_us - s->last_us;
    s->last_us = in->now_us;
    if (dt_us < 0 || dt_us > s->max_gap_us) {
        // Gap (sleep / off / reboot / clock jump): the current now says nothing about the gap. The cell
        // has been resting, so its voltage is close to OCV: re-anchor and restart rest tracking.
        s->soc_pct = ocv_pct;
        s->rest_since_us = -1;
        return s->soc_pct;
    }
    s->soc_pct += (float)in->ma * ((float)dt_us / 3.6e9f) / s->capacity_mah * 100.0f;

    // Rest: after 30 min of near-zero current, pull towards the OCV estimate.
    if (in->ma > -REST_MA && in->ma < REST_MA) {
        if (s->rest_since_us < 0) s->rest_since_us = in->now_us;
        if (in->now_us - s->rest_since_us >= REST_US) s->soc_pct += (ocv_pct - s->soc_pct) * REST_BLEND;
    } else {
        s->rest_since_us = -1;
    }
    // Anchors.
    if (in->charge_terminated && in->mv >= FULL_MV && in->ma >= -5) s->soc_pct = 100;
    if (in->mv <= OCV[0].mv && in->ma <= 0) s->soc_pct = 0;
    // Keep the counter from claiming full / empty against the voltage.
    if (s->soc_pct > 99 && in->mv < 4000) s->soc_pct = 99;
    s->soc_pct = clampf(s->soc_pct, 0, 100);
    return s->soc_pct;
}

uint8_t kiban_soc_display(kiban_soc_t *s)
{
    const int target = (int)lroundf(s->soc_pct);
    if (target > s->shown_pct) s->shown_pct++;
    else if (target < s->shown_pct) s->shown_pct--;
    return s->shown_pct;
}
