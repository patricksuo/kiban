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
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"
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
static void slog_continue(void);
static RTC_NOINIT_ATTR light_rec_t s_light;
static RTC_NOINIT_ATTR light_rec_t s_deep;   // filled at the boot that follows a deep-sleep wake
static RTC_DATA_ATTR int64_t s_deep_start_us;
static RTC_DATA_ATTR uint32_t s_deep_asked_ms;
#define MAGIC 0x6b696261

// Power-off test record: stage 1 = pulsing started, 2 = power stayed on, fell back to deep sleep.
typedef struct { uint32_t magic; int stage; } off_rec_t;
static RTC_NOINIT_ATTR off_rec_t s_off;
typedef struct { uint32_t magic; uint16_t out, cfg; } por_rec_t;
#define POR_MAGIC 0x42315f31   // distinct from MAGIC so older RTC_NOINIT records cannot alias it
static RTC_NOINIT_ATTR por_rec_t s_por;   // expander state seen at the last power-on boot (open item B1)

// Full-screen markers drawn directly in the 800x480 frame (orientation irrelevant).
static void draw_marker(bool off_screen)
{
    uint8_t *fb = heap_caps_malloc(KIBAN_EPD_FB_BYTES, MALLOC_CAP_SPIRAM);
    if (!fb) return;
    for (int y = 0; y < KIBAN_EPD_HEIGHT; y++)
        for (int x = 0; x < KIBAN_EPD_WIDTH; x++) {
            bool black;
            if (off_screen) black = !(x >= 300 && x < 500 && y >= 140 && y < 340);   // black, white square
            else black = ((x / 40) + (y / 40)) % 2 == 0;                              // checkerboard
            uint8_t *b = &fb[y * (KIBAN_EPD_WIDTH / 8) + x / 8];
            const uint8_t m = (uint8_t)(0x80 >> (x % 8));
            if (black) *b &= (uint8_t)~m; else *b |= m;
        }
    if (kiban_epd_init() == ESP_OK) {
        kiban_epd_show(fb, KIBAN_EPD_MODE_FULL, 0);
        kiban_epd_sleep();
    }
    free(fb);
}

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
    const kiban_board_boot_info_t bi = kiban_board_boot_info();
    if (esp_reset_reason() == ESP_RST_POWERON && bi.ioexp_valid)
        s_por = (por_rec_t){.magic = POR_MAGIC, .out = bi.ioexp_output, .cfg = bi.ioexp_config};
    if (s_por.magic == POR_MAGIC)
        printf("last power-on boot: expander output 0x%04x config 0x%04x (%s)\n", s_por.out, s_por.cfg,
               s_por.cfg == 0xffff ? "power-on default: the TCA9555 lost power" : "state kept across power-off");
    if (esp_reset_reason() == ESP_RST_POWERON) {
        printf("power-on reset: the board was really off (or first power). Drawing the checkerboard marker.\n");
        draw_marker(false);
    }
    if (s_off.magic == MAGIC) {
        printf("last poweroff: %s\n", s_off.stage == 2 ? "power stayed on after 5 s of pulses; fell back to deep sleep"
                                                      : "pulsing started, no fallback recorded");
        s_off.magic = 0;
    }
    kiban_rtc_wake_cancel();   // clear any armed RTC countdown / pending INT
    slog_continue();           // overnight log: sample and go back to sleep (does not return while active)
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

// poweroff [delay_s] [rtc_s]: draw the OFF marker, wait delay_s (time to unplug USB), optionally arm the
// RTC countdown, then pulse the power-switch chip for at most 5 s; fall back to deep sleep (POWER wake).
static int cmd_poweroff(int argc, char **argv)
{
    const int delay_s = argc > 1 ? atoi(argv[1]) : 0;
    const int rtc_s = argc > 2 ? atoi(argv[2]) : 0;
    printf("poweroff: OFF marker, delay %d s, RTC countdown %d s\n", delay_s, rtc_s);
    draw_marker(true);
    for (int i = delay_s; i > 0; i--) { printf("  %d\n", i); vTaskDelay(pdMS_TO_TICKS(1000)); }
    if (rtc_s > 0) kiban_rtc_wake_after((uint32_t)rtc_s);
    kiban_power_quiesce();
    s_off = (off_rec_t){.magic = MAGIC, .stage = 1};
    kiban_power_off(5000);
    s_off.stage = 2;
    printf("still powered after 5 s: deep sleep, wake with POWER\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    const kiban_wake_cfg_t w = {.timer_ms = 0, .power_key = true};
    kiban_power_deep_sleep(&w);
}

// ---- overnight sleep-current log -----------------------------------------------------------
// sleeplog start <interval_min> <count>: deep sleep with timer wake; at each wake one gauge sample is
// appended to the product NVS (survives anything), then straight back to sleep. sleeplog dump / stop.
typedef struct {
    uint32_t t_s;          // seconds since start (RTC timer, continuous across deep sleep)
    uint16_t mv;
    int16_t ma, ma_avg, standby_ma, temp_x10;
    uint16_t rm_mah;
} slog_sample_t;
#define SLOG_MAX 64

static esp_err_t slog_nvs(nvs_handle_t *h)
{
    esp_err_t err = nvs_flash_init_partition("nvs");
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    return nvs_open("sleeplog", NVS_READWRITE, h);
}

static void slog_take(nvs_handle_t h, int64_t start_us)
{
    slog_sample_t smp = {0};
    kiban_gauge_t g;
    if (kiban_gauge_read(&g) == ESP_OK) {
        uint8_t b[2] = {0};
        smp.mv = g.voltage_mv; smp.ma = g.current_ma; smp.ma_avg = g.avg_current_ma; smp.temp_x10 = g.temp_c_x10;
        smp.rm_mah = g.remaining_mah;
        (void)b;
    }
    smp.t_s = (uint32_t)((esp_rtc_get_time_us() - start_us) / 1000000);
    static slog_sample_t buf[SLOG_MAX];
    size_t len = sizeof buf;
    if (nvs_get_blob(h, "samples", buf, &len) != ESP_OK) len = 0;
    size_t n = len / sizeof(slog_sample_t);
    if (n < SLOG_MAX) { buf[n++] = smp; nvs_set_blob(h, "samples", buf, n * sizeof(slog_sample_t)); }
    nvs_commit(h);
}

// Called from probe_report_wake(): continue an active log after a timer wake.
static void slog_continue(void)
{
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP) return;
    nvs_handle_t h;
    if (slog_nvs(&h) != ESP_OK) return;
    uint32_t left = 0, interval = 0;
    int64_t start = 0;
    if (nvs_get_u32(h, "left", &left) != ESP_OK || !left || nvs_get_u32(h, "interval", &interval) != ESP_OK ||
        nvs_get_i64(h, "start", &start) != ESP_OK) { nvs_close(h); return; }
    slog_take(h, start);
    left--;
    nvs_set_u32(h, "left", left);
    nvs_commit(h);
    nvs_close(h);
    if (!left) { printf("sleeplog: finished\n"); return; }
    kiban_power_prepare();
    const kiban_wake_cfg_t w = {.timer_ms = interval * 1000, .power_key = true};
    kiban_power_deep_sleep(&w);
}

static int cmd_sleeplog(int argc, char **argv)
{
    nvs_handle_t h;
    esp_err_t err = slog_nvs(&h);
    if (err != ESP_OK) { printf("nvs: %s\n", esp_err_to_name(err)); return 1; }
    if (argc > 1 && !strcmp(argv[1], "dump")) {
        static slog_sample_t buf[SLOG_MAX];
        size_t len = sizeof buf;
        uint32_t left = 0;
        nvs_get_u32(h, "left", &left);
        if (nvs_get_blob(h, "samples", buf, &len) != ESP_OK) len = 0;
        printf("sleeplog: %u samples, %lu wakes left\n  t_min    mV     mA  avg_mA  RM_mAh  temp\n",
               (unsigned)(len / sizeof(slog_sample_t)), (unsigned long)left);
        for (size_t i = 0; i < len / sizeof(slog_sample_t); i++)
            printf("  %5lu  %5u  %5d  %6d  %6u  %d.%d\n", (unsigned long)(buf[i].t_s / 60), buf[i].mv, buf[i].ma,
                   buf[i].ma_avg, buf[i].rm_mah, buf[i].temp_x10 / 10, abs(buf[i].temp_x10 % 10));
        nvs_close(h);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "stop")) {
        nvs_set_u32(h, "left", 0); nvs_commit(h); nvs_close(h);
        printf("sleeplog stopped\n");
        return 0;
    }
    if (argc > 3 && !strcmp(argv[1], "start")) {
        const uint32_t interval_s = (uint32_t)atoi(argv[2]) * 60, count = (uint32_t)atoi(argv[3]);
        const int64_t start = esp_rtc_get_time_us();
        nvs_erase_key(h, "samples");
        nvs_set_u32(h, "interval", interval_s);
        nvs_set_u32(h, "left", count);
        nvs_set_i64(h, "start", start);
        slog_take(h, start);   // sample 0 before the first sleep
        nvs_commit(h);
        nvs_close(h);
        printf("sleeplog: %lu wakes every %lu min; deep sleep now (unplug USB)\n", (unsigned long)count,
               (unsigned long)(interval_s / 60));
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(100));
        kiban_power_prepare();
        const kiban_wake_cfg_t w = {.timer_ms = interval_s * 1000, .power_key = true};
        kiban_power_deep_sleep(&w);
    }
    printf("usage: sleeplog start <interval_min> <count> | dump | stop\n");
    nvs_close(h);
    return 1;
}

// Start the PCF8563 countdown interrupt and watch candidate lines for a change.
static int cmd_rtcint(int argc, char **argv)
{
    const int secs = argc > 1 ? atoi(argv[1]) : 5;
    const gpio_config_t in = {.pin_bit_mask = (1ULL << KIBAN_PIN_POWER_KEY), .mode = GPIO_MODE_INPUT};
    gpio_config(&in);
    esp_err_t err = kiban_rtc_wake_after((uint32_t)secs);
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
    kiban_rtc_wake_cancel();
    printf("RTC timer off\n");
    return 0;
}

void probe_register_power_cmds(void)
{
    const esp_console_cmd_t cmds[] = {
        {.command = "sleep", .help = "sleep light|deep <ms> [key]", .func = cmd_sleep},
        {.command = "poweroff", .help = "poweroff [delay_s] [rtc_s]: hardware power-off test (5 s max, then deep sleep)", .func = cmd_poweroff},
        {.command = "sleeplog", .help = "sleeplog start <interval_min> <count> | dump | stop", .func = cmd_sleeplog},
        {.command = "rtcint", .help = "rtcint [s]: arm the RTC countdown interrupt and watch where it shows up", .func = cmd_rtcint},
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
}
