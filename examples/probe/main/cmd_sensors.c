// P4 commands: bat, chg, rtc, imu.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kiban/sensors.h"
#include "probe_cmds.h"

static int cmd_bat(int argc, char **argv)
{
    kiban_gauge_t g;
    if (argc >= 3 && !strcmp(argv[1], "capacity")) {
        esp_err_t e = kiban_gauge_set_design_capacity((uint16_t)atoi(argv[2]), argc > 3 && !strcmp(argv[3], "force"));
        printf("set design capacity: %s\n", esp_err_to_name(e));
    }
    esp_err_t err = kiban_gauge_read(&g);
    if (err != ESP_OK) { printf("gauge: %s\n", esp_err_to_name(err)); return 1; }
    printf("voltage %u mV  current %d mA (avg %d)  temp %d.%d C\n", g.voltage_mv, g.current_ma, g.avg_current_ma,
           g.temp_c_x10 / 10, abs(g.temp_c_x10 % 10));
    printf("SOC %u %%  remaining %u / full %u / design %u mAh  cycles %u  SOH %u %%\n", g.soc_pct, g.remaining_mah,
           g.full_mah, g.design_mah, g.cycle_count, g.soh_pct);
    printf("gauge config: ChargingVoltage %u mV, ChargingCurrent %u mA  BatteryStatus 0x%04x\n", g.charge_voltage_mv,
           g.charge_current_ma, g.battery_status);
    return 0;
}

static const char *VBUS[] = {"none", "USB SDP", "USB CDP", "USB DCP", "unknown adapter", "non-standard adapter", "?", "OTG"};
static const char *CHRG[] = {"not charging / terminated", "pre-charge / CC", "CV", "top-off"};

static int cmd_chg(int argc, char **argv)
{
    kiban_charger_t c;
    if (argc == 3 && !strcmp(argv[1], "vreg")) {
        if (kiban_charger_read(&c) == ESP_OK) printf("before: 0x04=%02x 0x05=%02x (%lu mV)\n", c.regs[4], c.regs[5], (unsigned long)c.vreg_mv);
        esp_err_t e = kiban_charger_set_vreg_mv((uint32_t)atoi(argv[2]));
        printf("set_vreg: %s\n", esp_err_to_name(e));
        if (kiban_charger_read(&c) == ESP_OK) printf("after:  0x04=%02x 0x05=%02x (%lu mV)\n", c.regs[4], c.regs[5], (unsigned long)c.vreg_mv);
        return 0;
    }
    esp_err_t err = kiban_charger_read(&c);
    if (err != ESP_OK) { printf("charger: %s\n", esp_err_to_name(err)); return 1; }
    for (int r = 0; r < 0x40; r += 16) {
        printf("%02x:", r);
        for (int i = 0; i < 16; i++) printf(" %02x", c.regs[r + i]);
        printf("\n");
    }
    printf("VREG %lu mV  ICHG %lu mA  IINDPM %lu mA  VBUS %s  CHRG %s\n", (unsigned long)c.vreg_mv,
           (unsigned long)c.ichg_ma, (unsigned long)c.iindpm_ma, VBUS[c.vbus_stat & 7], CHRG[c.chrg_stat & 3]);
    return 0;
}

static int cmd_rtc(int argc, char **argv)
{
    (void)argc; (void)argv;
    kiban_rtc_time_t t;
    esp_err_t err = kiban_rtc_get(&t);
    if (err != ESP_OK) { printf("rtc: %s\n", esp_err_to_name(err)); return 1; }
    printf("%04d-%02d-%02d %02d:%02d:%02d (weekday %d)%s\n", t.year, t.month, t.day, t.hour, t.minute, t.second,
           t.weekday, t.integrity_lost ? "  VL set: clock integrity not guaranteed" : "");
    return 0;
}

static int cmd_imu(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 5;
    if (secs <= 0 || secs > 60) secs = 5;
    uint8_t who = 0;
    esp_err_t err = kiban_imu_init(&who);
    if (err != ESP_OK) { printf("imu: %s\n", esp_err_to_name(err)); return 1; }
    printf("WHO_AM_I 0x%02x\n", who);
    vTaskDelay(pdMS_TO_TICKS(50));
    for (int i = 0; i < secs * 4; i++) {
        kiban_imu_sample_t s;
        if (kiban_imu_read(&s) == ESP_OK) printf("x %5d  y %5d  z %5d mg\n", s.x_mg, s.y_mg, s.z_mg);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return 0;
}

void probe_register_sensor_cmds(void)
{
    const esp_console_cmd_t cmds[] = {
        {.command = "bat", .help = "bat [capacity <mAh> [force]]: fuel gauge readings; optionally program design capacity", .func = cmd_bat},
        {.command = "chg", .help = "chg [vreg <mv>]: charger register dump and decode; vreg sets the termination voltage (capped at 4200)", .func = cmd_chg},
        {.command = "rtc", .help = "read the RTC", .func = cmd_rtc},
        {.command = "imu", .help = "imu [seconds]: accelerometer samples at 4 Hz", .func = cmd_imu},
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
}
