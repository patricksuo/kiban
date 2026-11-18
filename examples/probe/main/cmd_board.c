// P1 commands: I2C scan, expander dump, rails, keys.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kiban/board.h"
#include "kiban/ioexp.h"
#include "kiban/keys.h"
#include "probe_cmds.h"

static const struct { uint8_t addr; const char *name; } KNOWN[] = {
    {0x15, "CST816S touch"}, {0x19, "SC7A20H accel"}, {0x20, "TCA9555 expander"},
    {0x51, "PCF8563 RTC"},   {0x55, "BQ27220 gauge"}, {0x6B, "CX25601N charger"},
};

static const char *known_name(uint8_t a)
{
    for (size_t i = 0; i < sizeof KNOWN / sizeof KNOWN[0]; i++)
        if (KNOWN[i].addr == a) return KNOWN[i].name;
    return "unknown";
}

static int cmd_i2c_scan(int argc, char **argv)
{
    (void)argc; (void)argv;
    i2c_master_bus_handle_t bus = kiban_board_i2c();
    if (!bus) { printf("i2c bus not initialised\n"); return 1; }
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(bus, a, 20) == ESP_OK) {
            printf("  0x%02x  %s\n", a, known_name(a));
            found++;
        }
    }
    printf("%d device(s). Note: CST816S may NACK unless the screen is being touched.\n", found);
    return 0;
}

static void print_bits(const char *label, uint16_t v)
{
    printf("%-9s 0x%04x  P1:", label, v);
    for (int i = 15; i >= 8; i--) printf("%d", (v >> i) & 1);
    printf(" P0:");
    for (int i = 7; i >= 0; i--) printf("%d", (v >> i) & 1);
    printf("\n");
}

static int cmd_ioexp(int argc, char **argv)
{
    (void)argc; (void)argv;
    static const struct { uint8_t reg; const char *name; } REGS[] = {
        {KIBAN_IOEXP_REG_INPUT, "input"}, {KIBAN_IOEXP_REG_OUTPUT, "output"},
        {KIBAN_IOEXP_REG_POLARITY, "polarity"}, {KIBAN_IOEXP_REG_CONFIG, "config"},
    };
    for (size_t i = 0; i < sizeof REGS / sizeof REGS[0]; i++) {
        uint16_t v;
        if (kiban_ioexp_read16(REGS[i].reg, &v) != ESP_OK) { printf("%s: read failed\n", REGS[i].name); return 1; }
        print_bits(REGS[i].name, v);
    }
    printf("config bit 1 = input. P0.0 usbmux P0.1 paswitch P0.4 amp P0.5 panel/sd P0.6 main P0.7 vol- P1.0 vol+ P1.1 touchrst P1.3 pwrpulse P1.4 accelint\n");
    return 0;
}

static int cmd_rail(int argc, char **argv)
{
    if (argc != 3) { printf("usage: rail <main|panel> <on|off>\n"); return 1; }
    kiban_rail_t r;
    if (!strcmp(argv[1], "main")) r = KIBAN_RAIL_MAIN;
    else if (!strcmp(argv[1], "panel")) r = KIBAN_RAIL_PANEL_SD;
    else { printf("unknown rail\n"); return 1; }
    bool on = !strcmp(argv[2], "on");
    if (!on && strcmp(argv[2], "off")) { printf("on|off\n"); return 1; }
    if (!on) printf("warning: what this rail feeds is not fully known; if I2C dies, run 'reboot'\n");
    esp_err_t err = kiban_board_rail_set(r, on);
    printf("%s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

// keys [seconds]: print debounced key events from kiban_keys (POWER, AI, VOL+/-, cover keys).
static int cmd_keys(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    if (secs <= 0 || secs > 120) secs = 10;
    static const char *NAME[] = {"POWER", "AI", "VOL+", "VOL-", "COVER_LEFT", "COVER_MIDDLE", "COVER_RIGHT"};
    static const char *ACT[] = {"press", "long", "release"};
    esp_err_t err = kiban_keys_init(NULL);
    if (err != ESP_OK) { printf("keys init: %s\n", esp_err_to_name(err)); return 1; }
    printf("watching keys for %d s\n", secs);
    const int64_t t0 = esp_timer_get_time(), end = t0 + (int64_t)secs * 1000000;
    while (esp_timer_get_time() < end) {
        kiban_key_event_t ev[8];
        const size_t n = kiban_keys_poll(esp_timer_get_time(), ev, 8);
        for (size_t i = 0; i < n; i++)
            printf("t=%6lld ms  %-12s %s%s\n", (long long)((ev[i].t_us - t0) / 1000), NAME[ev[i].key], ACT[ev[i].action],
                   ev[i].after_long ? " (after long)" : "");
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    return 0;
}

void probe_register_board_cmds(void)
{
    const esp_console_cmd_t cmds[] = {
        {.command = "i2c_scan", .help = "probe all 7-bit I2C addresses", .func = cmd_i2c_scan},
        {.command = "ioexp", .help = "dump TCA9555 registers", .func = cmd_ioexp},
        {.command = "rail", .help = "rail <main|panel> <on|off>", .func = cmd_rail},
        {.command = "keys", .help = "keys [seconds]: print debounced key events", .func = cmd_keys},
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
}
