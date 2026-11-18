// P7 commands: sleep tests and RTC interrupt routing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_console.h"
#include "esp_rtc_time.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kiban/board.h"
#include "kiban/epd.h"
#include "kiban/ioexp.h"
#include "kiban/power.h"
#include "kiban/sensors.h"
#include "probe_cmds.h"

// Survive the chip reset that opening the port causes on macOS.
typedef struct { uint32_t magic; int cause; int64_t asked_ms, slept_ms; } light_rec_t;
static RTC_NOINIT_ATTR light_rec_t s_light;
static RTC_NOINIT_ATTR light_rec_t s_deep;   // filled at the boot that follows a deep-sleep wake
static RTC_DATA_ATTR int64_t s_deep_start_us;
static RTC_DATA_ATTR uint32_t s_deep_asked_ms;
#define MAGIC 0x6b696261

static const char *cause_name(int c)
{
    switch (c) {
    case ESP_SLEEP_WAKEUP_TIMER: return "timer";
    case ESP_SLEEP_WAKEUP_GPIO: return "gpio (POWER)";
    case ESP_SLEEP_WAKEUP_EXT1: return "ext1 (POWER)";
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "none/undefined";
    default: return "other";
    }
}

void probe_report_wake(void)
{
    const uint32_t m = esp_sleep_get_wakeup_causes();
    int c = ESP_SLEEP_WAKEUP_UNDEFINED;
    for (int i = 0; i < 32; i++)
        if (m & (1u << i)) { c = i; break; }
    printf("boot: reset reason %d, wake causes 0x%lx (%s)\n", (int)esp_reset_reason(), (unsigned long)m, cause_name(c));
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP)
        s_deep = (light_rec_t){.magic = MAGIC, .cause = c, .asked_ms = s_deep_asked_ms,
                               .slept_ms = (esp_rtc_get_time_us() - s_deep_start_us) / 1000};
    if (s_deep.magic == MAGIC)
        printf("last deep sleep: asked %lld ms, slept %lld ms, cause %s\n", (long long)s_deep.asked_ms,
               (long long)s_deep.slept_ms, cause_name(s_deep.cause));
    if (s_light.magic == MAGIC)
        printf("last light sleep: asked %lld ms, slept %lld ms, cause %s\n", (long long)s_light.asked_ms,
               (long long)s_light.slept_ms, cause_name(s_light.cause));
}

static int cmd_sleep(int argc, char **argv)
{
    if (argc < 3) { printf("usage: sleep light|deep <ms> [key]\n"); return 1; }
    kiban_wake_cfg_t w = {.timer_ms = (uint32_t)atoi(argv[2]), .power_key = argc > 3 && !strcmp(argv[3], "key")};
    kiban_power_quiesce();
    if (!strcmp(argv[1], "light")) {
        printf("light sleep %lu ms%s ...\n", (unsigned long)w.timer_ms, w.power_key ? " (+POWER key)" : "");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(50));
        const int64_t t0 = esp_timer_get_time();
        const esp_sleep_wakeup_cause_t c = kiban_power_light_sleep(&w);
        s_light = (light_rec_t){.magic = MAGIC, .cause = c, .asked_ms = w.timer_ms,
                                .slept_ms = (esp_timer_get_time() - t0) / 1000};
        printf("woke: cause %s, slept %lld ms\n", cause_name(c), (long long)s_light.slept_ms);
        return 0;
    }
    if (!strcmp(argv[1], "deep")) {
        printf("deep sleep %lu ms%s; result is printed at the next boot\n", (unsigned long)w.timer_ms,
               w.power_key ? " (+POWER key)" : "");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(50));
        s_deep_asked_ms = w.timer_ms;
        s_deep_start_us = esp_rtc_get_time_us();
        kiban_power_deep_sleep(&w);
    }
    printf("light|deep\n");
    return 1;
}

// Start the PCF8563 countdown interrupt and watch candidate lines for a change.
static int cmd_rtcint(int argc, char **argv)
{
    const int secs = argc > 1 ? atoi(argv[1]) : 5;
    const gpio_config_t in = {.pin_bit_mask = (1ULL << KIBAN_PIN_POWER_KEY), .mode = GPIO_MODE_INPUT};
    gpio_config(&in);
    esp_err_t err = kiban_rtc_timer_irq((uint8_t)secs);
    if (err != ESP_OK) { printf("rtc: %s\n", esp_err_to_name(err)); return 1; }
    printf("RTC countdown %d s armed; watching expander inputs, GPIO1 (touch INT), GPIO2 (expander INT), GPIO3 (POWER)\n", secs);
    const int64_t t0 = esp_timer_get_time(), end = t0 + (int64_t)(secs + 5) * 1000000;
    uint32_t last = 0xFFFFFFFF;
    while (esp_timer_get_time() < end) {
        uint16_t ex = 0;
        kiban_ioexp_read16(KIBAN_IOEXP_REG_INPUT, &ex);
        const uint32_t st = ex | ((uint32_t)gpio_get_level(1) << 16) | ((uint32_t)gpio_get_level(2) << 17) |
                            ((uint32_t)gpio_get_level(KIBAN_PIN_POWER_KEY) << 18);
        if (st != last) {
            printf("t=%6lld ms  expander in=0x%04x  GPIO1=%lu GPIO2=%lu GPIO3=%lu\n",
                   (long long)((esp_timer_get_time() - t0) / 1000), ex, (unsigned long)((st >> 16) & 1),
                   (unsigned long)((st >> 17) & 1), (unsigned long)((st >> 18) & 1));
            last = st;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    kiban_rtc_timer_off();
    printf("RTC timer off\n");
    return 0;
}

void probe_register_power_cmds(void)
{
    const esp_console_cmd_t cmds[] = {
        {.command = "sleep", .help = "sleep light|deep <ms> [key]", .func = cmd_sleep},
        {.command = "rtcint", .help = "rtcint [s]: arm the RTC countdown interrupt and watch where it shows up", .func = cmd_rtcint},
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
}
