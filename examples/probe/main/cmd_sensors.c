// P4 commands: bat, chg, battery (poll), rtc, imu.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kiban/battery.h"
#include "kiban/board.h"
#include "kiban/charger.h"
#include "kiban/sensors.h"
#include "probe_cmds.h"

static int cmd_bat(int argc, char **argv)
{
    (void)argc; (void)argv;
    kiban_gauge_t g;
    esp_err_t err = kiban_gauge_read(&g);
    if (err != ESP_OK) { printf("gauge: %s\n", esp_err_to_name(err)); return 1; }
    printf("voltage %u mV  current %d mA (avg %d)  temp %d.%d C\n", g.voltage_mv, g.current_ma, g.avg_current_ma,
           g.temp_c_x10 / 10, abs(g.temp_c_x10 % 10));
    printf("gauge model (3000 mAh default, not meaningful): SOC %u %%  remaining %u / full %u / design %u mAh\n",
           g.soc_pct, g.remaining_mah, g.full_mah, g.design_mah);
    printf("BatteryStatus 0x%04x  OperationStatus 0x%04x\n", g.battery_status, g.operation_status);
    return 0;
}

static const char *VBUS[] = {"none", "USB SDP", "USB CDP", "USB DCP", "unknown adapter", "non-standard adapter", "?", "OTG"};
static const char *CHRG[] = {"not charging / terminated", "pre-charge / CC", "CV", "top-off"};

static int cmd_chg(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "dump")) {
        uint8_t r[64];
        esp_err_t err = kiban_charger_dump(r);
        if (err != ESP_OK) { printf("dump: %s\n", esp_err_to_name(err)); return 1; }
        for (int i = 0; i < 64; i += 16) {
            printf("%02x:", i);
            for (int j = 0; j < 16; j++) printf(" %02x", r[i + j]);
            printf("\n");
        }
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "apply")) {
        kiban_charge_policy_t p = KIBAN_CHARGE_POLICY_DEFAULT();
        if (argc > 2) p.ichg_ma = (uint32_t)atoi(argv[2]);
        printf("apply: %s\n", esp_err_to_name(kiban_charger_apply(&p)));
    }
    kiban_charger_status_t c;
    esp_err_t err = kiban_charger_status(&c);
    if (err != ESP_OK) { printf("charger: %s\n", esp_err_to_name(err)); return 1; }
    printf("VREG %lu mV  ICHG %lu mA  IINDPM %lu mA  EN_CHG %d  HIZ %d  EN_TERM %d  WDT %u\n",
           (unsigned long)c.vreg_mv, (unsigned long)c.ichg_ma, (unsigned long)c.iindpm_ma, c.en_chg, c.hiz,
           c.en_term, c.wdt);
    printf("CHRG %s  VBUS %s  0x14=%02x 0x16=%02x 0x1a=%02x 0x1e=%02x  policy_ok %d  board_charger_ok %d\n",
           CHRG[c.chrg_stat & 3], VBUS[c.vbus_stat & 7], c.reg14, c.reg16, c.reg1a, c.reg1e,
           kiban_charger_policy_ok(), kiban_board_charger_ok());
    return 0;
}

static const char *STATE[] = {"discharging", "charging", "full"};

// battery [seconds]: poll every 5 s and print the combined battery state.
static int cmd_battery(int argc, char **argv)
{
    static kiban_soc_t soc;
    static bool init;
    if (!init) { kiban_soc_init(&soc, KIBAN_BATTERY_CAPACITY_MAH); init = true; }
    const int secs = argc > 1 ? atoi(argv[1]) : 10;
    for (int t = 0; t <= secs; t += 5) {
        kiban_battery_t b;
        esp_err_t err = kiban_battery_poll(&soc, esp_timer_get_time(), &b);
        if (err != ESP_OK) { printf("poll: %s\n", esp_err_to_name(err)); return 1; }
        printf("%-11s %4u mV %5d mA (avg %5d)  %d.%d C  SOC %.1f %% shown %u %%%s%s%s\n", STATE[b.state],
               b.gauge.voltage_mv, b.gauge.current_ma, b.gauge.avg_current_ma, b.gauge.temp_c_x10 / 10,
               abs(b.gauge.temp_c_x10 % 10), (double)b.soc_pct, b.display_pct, b.critical ? "  CRITICAL" : "",
               b.temp_blocked ? "  TEMP-BLOCKED" : "", b.charger_ok ? "" : "  CHARGER-NOT-OK");
        if (t + 5 <= secs) vTaskDelay(pdMS_TO_TICKS(5000));
    }
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
        {.command = "bat", .help = "fuel gauge readings", .func = cmd_bat},
        {.command = "chg", .help = "chg [dump | apply [ichg_ma]]: charger status / register dump / re-apply policy", .func = cmd_chg},
        {.command = "battery", .help = "battery [seconds]: combined battery state every 5 s", .func = cmd_battery},
        {.command = "rtc", .help = "read the RTC", .func = cmd_rtc},
        {.command = "imu", .help = "imu [seconds]: accelerometer samples at 4 Hz", .func = cmd_imu},
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
}
