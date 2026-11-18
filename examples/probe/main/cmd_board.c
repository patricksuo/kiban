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

// Print raw key levels whenever they change, for N seconds (default 10).
static int cmd_keys(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    if (secs <= 0 || secs > 120) secs = 10;
    const gpio_config_t in = {.pin_bit_mask = (1ULL << KIBAN_PIN_BOOT_KEY) | (1ULL << KIBAN_PIN_POWER_KEY),
                              .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_DISABLE};
    gpio_config(&in);
    printf("watching BOOT(GPIO0) POWER(GPIO3) VOL-(P0.7) VOL+(P1.0) for %d s; raw levels, 1=high\n", secs);
    int last = -1;
    const int64_t end = esp_timer_get_time() + (int64_t)secs * 1000000;
    while (esp_timer_get_time() < end) {
        uint16_t in16 = 0xFFFF;
        bool ok = kiban_ioexp_read16(KIBAN_IOEXP_REG_INPUT, &in16) == ESP_OK;
        int state = (gpio_get_level(KIBAN_PIN_BOOT_KEY) << 0) | (gpio_get_level(KIBAN_PIN_POWER_KEY) << 1) |
                    (((in16 >> KIBAN_IOEXP_VOL_DOWN) & 1) << 2) | (((in16 >> KIBAN_IOEXP_VOL_UP) & 1) << 3) |
                    ((ok ? 0 : 1) << 4);
        if (state != last) {
            printf("t=%6lld ms  BOOT=%d POWER=%d VOL-=%d VOL+=%d%s\n",
                   (long long)((esp_timer_get_time() - (end - (int64_t)secs * 1000000)) / 1000),
                   state & 1, (state >> 1) & 1, (state >> 2) & 1, (state >> 3) & 1, ok ? "" : "  (expander read failed)");
            last = state;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return 0;
}

void probe_register_board_cmds(void)
{
    const esp_console_cmd_t cmds[] = {
        {.command = "i2c_scan", .help = "probe all 7-bit I2C addresses", .func = cmd_i2c_scan},
        {.command = "ioexp", .help = "dump TCA9555 registers", .func = cmd_ioexp},
        {.command = "rail", .help = "rail <main|panel> <on|off>", .func = cmd_rail},
        {.command = "keys", .help = "keys [seconds]: print raw key levels on change", .func = cmd_keys},
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
}
