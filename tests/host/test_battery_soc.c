// Host test: cc -I../../components/kiban/include test_battery_soc.c ../../components/kiban/src/battery_soc.c -lm
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "kiban/battery_soc.h"

static int64_t S(double s) { return (int64_t)(s * 1e6); }

int main(void)
{
    kiban_soc_t s;
    // 1. First sample uses the OCV table (with IR compensation).
    kiban_soc_init(&s, 1000);
    float p = kiban_soc_update(&s, &(kiban_soc_sample_t){.mv = 3840, .ma = 0, .now_us = 0});
    assert(fabsf(p - 55) < 0.5f);
    // 2. Discharging 100 mA for one hour removes 10 % of 1000 mAh.
    for (int i = 1; i <= 3600; i++) kiban_soc_update(&s, &(kiban_soc_sample_t){.mv = 3800, .ma = -100, .now_us = S(i)});
    assert(fabsf(s.soc_pct - 45) < 0.5f);
    // 3. Charge termination at 4.2 V anchors to 100.
    kiban_soc_update(&s, &(kiban_soc_sample_t){.mv = 4195, .ma = 0, .charge_terminated = true, .now_us = S(3601)});
    assert(s.soc_pct == 100);
    // 4. Terminated status while discharging on battery does not force 100.
    kiban_soc_restore(&s, 50, S(4000));
    kiban_soc_update(&s, &(kiban_soc_sample_t){.mv = 4160, .ma = -45, .charge_terminated = true, .now_us = S(4001)});
    assert(s.soc_pct < 51);
    // 5. Long rest pulls towards OCV.
    kiban_soc_restore(&s, 80, S(0));
    for (int m = 1; m <= 60; m++) kiban_soc_update(&s, &(kiban_soc_sample_t){.mv = 3840, .ma = 0, .now_us = S(m * 60)});
    assert(s.soc_pct < 60);
    // 6. Display moves 1 % per call.
    kiban_soc_restore(&s, 50, 0);
    s.soc_pct = 53;
    assert(kiban_soc_display(&s) == 51 && kiban_soc_display(&s) == 52 && kiban_soc_display(&s) == 53);
    // 7. A long gap (8 h of deep sleep / power-off) is not integrated with the wake-time current;
    //    the estimate re-anchors to the voltage table instead.
    kiban_soc_init(&s, 1000);
    kiban_soc_update(&s, &(kiban_soc_sample_t){.mv = 4020, .ma = 0, .now_us = S(0)});          // 85 %
    kiban_soc_update(&s, &(kiban_soc_sample_t){.mv = 4020, .ma = -45, .now_us = S(60)});       // normal step
    const float before = s.soc_pct;
    kiban_soc_update(&s, &(kiban_soc_sample_t){.mv = 4010, .ma = -45, .now_us = S(60 + 8 * 3600)});
    assert(s.soc_pct > before - 5);   // not -36 %
    // 8. A clock reset (time going backwards, e.g. after reboot) is a gap too.
    kiban_soc_update(&s, &(kiban_soc_sample_t){.mv = 4010, .ma = -45, .now_us = S(5)});
    assert(s.soc_pct > 70);
    printf("battery_soc: all tests passed\n");
    return 0;
}
