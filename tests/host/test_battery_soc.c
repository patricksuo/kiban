// Host test for battery_soc. Build and run:
//   cc -Wall -Wextra -Werror -I components/kiban/include tests/host/test_battery_soc.c \
//      components/kiban/src/battery_soc.c -lm -o /tmp/test_soc && /tmp/test_soc
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "kiban/battery_soc.h"

static int failures;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
            failures++;                                                          \
        }                                                                        \
    } while (0)
#define NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))

static int64_t S(double s) { return (int64_t)(s * 1e6); }

// Discharging/resting sample with the same instantaneous and average current, vreg 4200.
static kiban_soc_sample_t smp(uint16_t mv, int16_t ma, double t_s)
{
    return (kiban_soc_sample_t){.mv = mv, .ma_now = ma, .ma_avg = ma, .vreg_mv = 4200, .now_us = S(t_s)};
}

static void fresh(kiban_soc_t *s, float soc)
{
    kiban_soc_init(s, 1000);
    kiban_soc_restore(s, soc, 0);
}

int main(void)
{
    kiban_soc_t s;

    // OCV table: endpoints and monotonicity.
    CHECK(kiban_soc_from_ocv(3200) == 0 && kiban_soc_from_ocv(4300) == 100 && kiban_soc_from_ocv(NAN) == 0);
    for (int mv = 3300; mv < 4200; mv += 5) CHECK(kiban_soc_from_ocv(mv + 5) >= kiban_soc_from_ocv(mv));

    // First sample: OCV with IR compensation (discharging 100 mA at 0.15 ohm -> +15 mV).
    kiban_soc_init(&s, 1000);
    { kiban_soc_sample_t c = smp(3825, -100, 0); kiban_soc_update(&s, &c); }
    NEAR(s.soc_pct, 55, 0.5);
    // Charging at +200 mA: compensation goes the other way (-30 mV).
    kiban_soc_init(&s, 1000);
    { kiban_soc_sample_t c = smp(3870, 200, 0); c.charging = true; kiban_soc_update(&s, &c); }
    NEAR(s.soc_pct, 55, 0.5);

    // Discharge 100 mA for 1 h at 1 s steps: -10 % of 1000 mAh.
    fresh(&s, 55);
    for (int i = 1; i <= 3600; i++) { kiban_soc_sample_t c = smp(3800, -100, i); kiban_soc_update(&s, &c); }
    NEAR(s.soc_pct, 45, 0.3);
    // Same at 60 s steps.
    fresh(&s, 55);
    for (int i = 1; i <= 60; i++) { kiban_soc_sample_t c = smp(3800, -100, i * 60); kiban_soc_update(&s, &c); }
    NEAR(s.soc_pct, 45, 0.3);

    // Charging integrates upwards and is capped at 99 until termination.
    fresh(&s, 90);
    for (int i = 1; i <= 3600; i++) {
        kiban_soc_sample_t c = smp(4150, 480, i);
        c.charging = true;
        kiban_soc_update(&s, &c);
    }
    NEAR(s.soc_pct, 99, 0.01);
    // Termination at >= vreg - 80 mV with no discharge -> 100.
    { kiban_soc_sample_t c = smp(4190, 0, 3601); c.terminated = true; kiban_soc_update(&s, &c); }
    NEAR(s.soc_pct, 100, 0.01);
    // Boundary: 4119 mV is below vreg - 80 -> no anchor.
    fresh(&s, 90);
    { kiban_soc_sample_t c = smp(4119, 0, 1); c.terminated = true; kiban_soc_update(&s, &c); }
    CHECK(s.soc_pct < 100);
    // "Terminated" while discharging (-6 mA) does not anchor.
    fresh(&s, 90);
    { kiban_soc_sample_t c = smp(4190, -6, 1); c.terminated = true; kiban_soc_update(&s, &c); }
    CHECK(s.soc_pct < 100);

    // Declared gap: 8 h asleep at 3 mA = -2.4 %, then a 0.5 blend towards OCV (resting current at wake).
    fresh(&s, 85);
    { kiban_soc_sample_t c = smp(4020, -5, 10); kiban_soc_update(&s, &c); }
    kiban_soc_resume(&s, 8 * 3600);
    { kiban_soc_sample_t c = smp(4020, -5, 12); kiban_soc_update(&s, &c); }   // dt only 2 s (reboot case)
    {
        const double ocv = kiban_soc_from_ocv(4020 + 5 * 0.15f);
        NEAR(s.soc_pct, (85 - 2.4) + (ocv - (85 - 2.4)) * 0.5, 0.3);
    }
    // Undeclared gap (dt > max_gap): gap = dt, waking current large -> weight 0.2.
    fresh(&s, 85);
    { kiban_soc_sample_t c = smp(4020, -45, 10); kiban_soc_update(&s, &c); }
    { kiban_soc_sample_t c = smp(3990, -45, 10 + 8 * 3600); kiban_soc_update(&s, &c); }
    {
        const double ocv = kiban_soc_from_ocv(3990 + 45 * 0.15f);
        NEAR(s.soc_pct, (85 - 2.4) + (ocv - (85 - 2.4)) * 0.2, 0.3);
        CHECK(s.soc_pct > 75);   // not -36 % as with naive integration
    }
    // Time going backwards (reboot without resume) is a gap of unknown length: no sleep subtraction.
    fresh(&s, 85);
    { kiban_soc_sample_t c = smp(4020, -5, 100); kiban_soc_update(&s, &c); }
    { kiban_soc_sample_t c = smp(4020, -5, 5); kiban_soc_update(&s, &c); }
    {
        const double ocv = kiban_soc_from_ocv(4020 + 5 * 0.15f);
        NEAR(s.soc_pct, 85 + (ocv - 85) * 0.5, 0.3);
    }

    // A single voltage dip does not zero the estimate; three consecutive low samples do.
    fresh(&s, 30);
    { kiban_soc_sample_t c = smp(3290, -20, 1); kiban_soc_update(&s, &c); }
    CHECK(s.soc_pct > 0);
    { kiban_soc_sample_t c = smp(3750, -20, 2); kiban_soc_update(&s, &c); }
    CHECK(s.soc_pct > 0 && !s.critical);
    for (int i = 3; i <= 5; i++) { kiban_soc_sample_t c = smp(3290, -20, i); kiban_soc_update(&s, &c); }
    NEAR(s.soc_pct, 0, 0.01);
    CHECK(s.critical);

    // OCV band: an overestimate is pulled into OCV + 15 while discharging.
    fresh(&s, 80);
    { kiban_soc_sample_t c = smp(3770, -10, 1); kiban_soc_update(&s, &c); }
    CHECK(s.soc_pct <= kiban_soc_from_ocv(3770 + 1.5f) + 15.01);

    // Rest: after 30 min of < 10 mA, converge towards OCV (tau 600 s) at 60 s sampling.
    fresh(&s, 70);
    for (int m = 1; m <= 120; m++) { kiban_soc_sample_t c = smp(3840, -2, m * 60); kiban_soc_update(&s, &c); }
    NEAR(s.soc_pct, kiban_soc_from_ocv(3840 + 0.3f), 1.5);
    // Rest interrupted by a load restarts the timer: no pull before 30 min of fresh rest.
    fresh(&s, 70);
    for (int m = 1; m <= 20; m++) { kiban_soc_sample_t c = smp(3840, -2, m * 60); kiban_soc_update(&s, &c); }
    { kiban_soc_sample_t c = smp(3830, -60, 21 * 60); kiban_soc_update(&s, &c); }
    for (int m = 22; m <= 40; m++) { kiban_soc_sample_t c = smp(3840, -2, m * 60); kiban_soc_update(&s, &c); }
    CHECK(s.soc_pct > 68);

    // Display: one step per 20 s, deadband, no rise while discharging, no fall while charging.
    fresh(&s, 50);
    s.soc_pct = 47;
    CHECK(kiban_soc_display(&s, S(10), false) == 50);   // < 20 s since restore
    CHECK(kiban_soc_display(&s, S(21), false) == 49);
    CHECK(kiban_soc_display(&s, S(30), false) == 49);   // rate limit
    CHECK(kiban_soc_display(&s, S(42), false) == 48);
    s.soc_pct = 48.5;                                   // rising while discharging: held
    CHECK(kiban_soc_display(&s, S(70), false) == 48);
    CHECK(kiban_soc_display(&s, S(100), true) == 48);   // +0.5 is inside the deadband
    s.soc_pct = 52;
    CHECK(kiban_soc_display(&s, S(130), true) == 49);
    s.soc_pct = 10;
    CHECK(kiban_soc_display(&s, S(160), true) == 49);   // no fall while charging
    s.soc_pct = 100;                                    // terminated: may climb without "charging"
    CHECK(kiban_soc_display(&s, S(190), false) == 50);

    // Bad inputs.
    kiban_soc_init(&s, 0);
    CHECK(s.capacity_mah == KIBAN_BATTERY_CAPACITY_MAH);
    kiban_soc_restore(&s, NAN, 0);
    CHECK(!s.valid);
    kiban_soc_restore(&s, 150, 0);
    CHECK(!s.valid);

    if (failures) {
        printf("battery_soc: %d failure(s)\n", failures);
        return 1;
    }
    printf("battery_soc: all tests passed\n");
    return 0;
}
