// P3 commands: raw touch watch, and a guided on-screen touch test.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kiban/epd.h"
#include "kiban/touch.h"
#include "probe_cmds.h"

static const char *EV[] = {"down", "up", "contact", "none"};

// ---- drawing in portrait coordinates -------------------------------------------------------
// Measured (research/003 P2): portrait (px, py) with the cover keys at the bottom maps to the
// 800x480 frame as fx = py, fy = 479 - px. Frame bit = 1 is white, MSB = leftmost pixel.
#define PW 480
#define PH 800

static uint8_t *s_fb;

static void px_set(int px, int py, bool black)
{
    if (px < 0 || px >= PW || py < 0 || py >= PH) return;
    const int fx = py, fy = (PW - 1) - px;
    uint8_t *b = &s_fb[fy * (KIBAN_EPD_WIDTH / 8) + fx / 8];
    const uint8_t m = (uint8_t)(0x80 >> (fx % 8));
    if (black) *b &= (uint8_t)~m; else *b |= m;
}

static void rect(int x0, int y0, int x1, int y1)
{
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) px_set(x, y, true);
}

static void line(int x0, int y0, int x1, int y1, int thick)
{
    const int dx = abs(x1 - x0), dy = abs(y1 - y0), n = dx > dy ? dx : dy;
    for (int i = 0; i <= n; i++) {
        const int x = n ? x0 + (x1 - x0) * i / n : x0, y = n ? y0 + (y1 - y0) * i / n : y0;
        rect(x - thick / 2, y - thick / 2, x + thick / 2, y + thick / 2);
    }
}

// Ring + crosshair centred on (cx, cy).
static void target(int cx, int cy)
{
    for (int r = 26; r <= 32; r++)
        for (int a = 0; a < 720; a++) {
            const float rad = (float)a * 3.14159265f / 360.0f;
            px_set(cx + (int)lroundf((float)r * cosf(rad)), cy + (int)lroundf((float)r * sinf(rad)), true);
        }
    line(cx - 44, cy, cx + 44, cy, 3);
    line(cx, cy - 44, cx, cy + 44, 3);
}

// Downward arrow from (x, y0) to (x, y1), head at y1.
static void arrow_down(int x, int y0, int y1)
{
    line(x, y0, x, y1 - 30, 7);
    for (int i = 0; i < 40; i++) rect(x - i, y1 - i, x + i, y1 - i);
}

static void dot(int x, int y) { rect(x - 6, y - 6, x + 6, y + 6); }

// ---- raw watch -----------------------------------------------------------------------------
static int cmd_touch(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 20;
    if (secs <= 0 || secs > 120) secs = 20;
    esp_err_t err = kiban_touch_init();
    if (err != ESP_OK) { printf("init: %s\n", esp_err_to_name(err)); return 1; }
    printf("watching touch for %d s\n", secs);
    const int64_t t0 = esp_timer_get_time(), end = t0 + (int64_t)secs * 1000000;
    kiban_touch_point_t last = {0};
    bool had = false;
    while (esp_timer_get_time() < end) {
        kiban_touch_point_t p;
        if (kiban_touch_read(&p) == ESP_OK &&
            (!had || p.fingers != last.fingers || p.x != last.x || p.y != last.y || p.event != last.event)) {
            printf("t=%6lld ms  fingers=%u ev=%-7s x=%3u y=%3u gesture=0x%02x\n",
                   (long long)((esp_timer_get_time() - t0) / 1000), p.fingers, EV[p.event & 3], p.x, p.y, p.gesture);
            last = p;
            had = true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return 0;
}

// ---- guided test ---------------------------------------------------------------------------
typedef enum { STEP_TAP, STEP_SWIPE_DOWN, STEP_COVER } step_kind_t;
typedef struct { step_kind_t kind; int x, y; const char *name; } step_t;

static const step_t STEPS[] = {
    {STEP_TAP, 50, 50, "tap the top-left target"},
    {STEP_TAP, 430, 750, "tap the bottom-right target"},
    {STEP_TAP, 240, 400, "tap the centre target"},
    {STEP_SWIPE_DOWN, 240, 0, "swipe down along the arrow"},
    {STEP_COVER, 80, 0, "press the cover key below the triangle (left)"},
    {STEP_COVER, 240, 0, "press the cover key below the triangle (middle)"},
    {STEP_COVER, 400, 0, "press the cover key below the triangle (right)"},
};

static void draw_step(const step_t *s)
{
    switch (s->kind) {
    case STEP_TAP: target(s->x, s->y); break;
    case STEP_SWIPE_DOWN: arrow_down(s->x, 150, 650); break;
    case STEP_COVER: for (int i = 0; i < 36; i++) rect(s->x - 36 + i, 760 + i, s->x + 36 - i, 760 + i); break;
    }
}

// Wait for one gesture: first and last contact point until release. Returns false on timeout.
static bool wait_gesture(int timeout_s, kiban_touch_point_t *first, kiban_touch_point_t *lastp, int *samples)
{
    const int64_t end = esp_timer_get_time() + (int64_t)timeout_s * 1000000;
    bool down = false;
    int idle = 0;
    *samples = 0;
    while (esp_timer_get_time() < end) {
        kiban_touch_point_t p;
        const bool ok = kiban_touch_read(&p) == ESP_OK && p.fingers > 0;
        if (ok) {
            if (!down) { *first = p; down = true; }
            *lastp = p;
            (*samples)++;
            idle = 0;
        } else if (down && ++idle >= 5) {  // ~50 ms without contact = released
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return down;
}

static int cmd_touch_test(int argc, char **argv)
{
    // "touch_test keys" runs only the cover-key steps.
    const int first_step = (argc > 1 && !strcmp(argv[1], "keys")) ? 4 : 0;
    esp_err_t err = kiban_touch_init();
    if (err != ESP_OK) { printf("touch init: %s\n", esp_err_to_name(err)); return 1; }
    if (!s_fb) s_fb = heap_caps_malloc(KIBAN_EPD_FB_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_fb) { printf("no memory\n"); return 1; }
    err = kiban_epd_init();
    if (err != ESP_OK) { printf("epd init: %s\n", esp_err_to_name(err)); return 1; }

    const int n = sizeof STEPS / sizeof STEPS[0];
    memset(s_fb, 0xFF, KIBAN_EPD_FB_BYTES);
    draw_step(&STEPS[first_step]);
    err = kiban_epd_show(s_fb, KIBAN_EPD_MODE_FULL, 0);
    if (err != ESP_OK) { printf("epd: %s\n", esp_err_to_name(err)); return 1; }

    for (int i = first_step; i < n; i++) {
        const step_t *s = &STEPS[i];
        printf("step %d/%d: %s (15 s)\n", i + 1, n, s->name);
        kiban_touch_point_t a = {0}, b = {0};
        int samples = 0;
        const bool got = wait_gesture(15, &a, &b, &samples);
        if (got) {
            printf("  first x=%3u y=%3u  last x=%3u y=%3u  samples=%d gesture=0x%02x", a.x, a.y, b.x, b.y, samples,
                   b.gesture);
            if (s->kind == STEP_TAP) printf("  error dx=%+d dy=%+d", (int)a.x - s->x, (int)a.y - s->y);
            printf("\n");
        } else {
            printf("  timeout, skipped\n");
        }
        // Next screen: a dot where the finger landed (and a trail for the swipe), then the next guide.
        memset(s_fb, 0xFF, KIBAN_EPD_FB_BYTES);
        if (got && a.y < PH) dot(a.x, a.y);
        if (got && s->kind == STEP_SWIPE_DOWN && b.y < PH) { dot(b.x, b.y); line(a.x, a.y, b.x, b.y, 3); }
        if (i + 1 < n) draw_step(&STEPS[i + 1]);
        err = kiban_epd_show(s_fb, KIBAN_EPD_MODE_PARTIAL, 0);
        if (err != ESP_OK) { printf("epd: %s\n", esp_err_to_name(err)); return 1; }
    }
    printf("touch test done\n");
    return 0;
}

void probe_register_touch_cmds(void)
{
    const esp_console_cmd_t cmds[] = {
        {.command = "touch", .help = "touch [seconds]: print raw touch reports", .func = cmd_touch},
        {.command = "touch_test", .help = "touch_test [keys]: guided touch test with on-screen targets", .func = cmd_touch_test},
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
}
