// P3 command: watch touch reports.
#include <stdio.h>
#include <stdlib.h>

#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kiban/touch.h"
#include "probe_cmds.h"

static const char *EV[] = {"down", "up", "contact", "none"};

// Cover keys per vendor firmware: HOME (80,900), NEXT (240,900), PREV (400,900).
static const char *zone(uint16_t x, uint16_t y)
{
    if (y < 800) return "panel";
    if (x < 160) return "cover:HOME?";
    if (x < 320) return "cover:NEXT?";
    return "cover:PREV?";
}

// touch [seconds] [all]: poll every 10 ms, print each report that differs from the previous one.
static int cmd_touch(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 20;
    if (secs <= 0 || secs > 120) secs = 20;
    esp_err_t err = kiban_touch_init();
    if (err != ESP_OK) { printf("init: %s\n", esp_err_to_name(err)); return 1; }
    printf("watching touch for %d s (x 0..479, y 0..799 = panel, y >= 800 = cover keys)\n", secs);
    const int64_t t0 = esp_timer_get_time(), end = t0 + (int64_t)secs * 1000000;
    kiban_touch_point_t last = {0};
    int nacks = 0, reads = 0;
    bool had = false;
    while (esp_timer_get_time() < end) {
        kiban_touch_point_t p;
        reads++;
        if (kiban_touch_read(&p) != ESP_OK) {
            nacks++;
        } else if (!had || p.fingers != last.fingers || p.x != last.x || p.y != last.y || p.event != last.event ||
                   p.gesture != last.gesture) {
            printf("t=%6lld ms  fingers=%u ev=%-7s x=%3u y=%3u gesture=0x%02x int=%d  %s\n",
                   (long long)((esp_timer_get_time() - t0) / 1000), p.fingers, EV[p.event & 3], p.x, p.y, p.gesture,
                   kiban_touch_int_active(), p.fingers ? zone(p.x, p.y) : "");
            last = p;
            had = true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    printf("done: %d reads, %d failed (NACK while idle is expected)\n", reads, nacks);
    return 0;
}

void probe_register_touch_cmds(void)
{
    const esp_console_cmd_t cmd = {.command = "touch", .help = "touch [seconds]: print touch reports", .func = cmd_touch};
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
