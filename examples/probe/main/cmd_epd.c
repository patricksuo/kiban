// P2 commands: SSD1677 e-paper panel.
#include <stdio.h>
#include <string.h>

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "kiban/board.h"
#include "kiban/epd.h"
#include "probe_cmds.h"

static uint8_t *s_fb;    // 1bpp, PSRAM
static uint8_t *s_fb2;   // 2bpp, PSRAM

static bool alloc_fbs(void)
{
    if (!s_fb) s_fb = heap_caps_malloc(KIBAN_EPD_FB_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_fb2) s_fb2 = heap_caps_malloc(KIBAN_EPD_FB2_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_fb || !s_fb2) { printf("framebuffer allocation in PSRAM failed\n"); return false; }
    return true;
}

// 1bpp: bit 1 = white.
static void rect_black(int x0, int y0, int w, int h)
{
    for (int y = y0; y < y0 + h && y < KIBAN_EPD_HEIGHT; y++)
        for (int x = x0; x < x0 + w && x < KIBAN_EPD_WIDTH; x++)
            s_fb[y * (KIBAN_EPD_WIDTH / 8) + x / 8] &= (uint8_t)~(0x80 >> (x & 7));
}

static bool make_pattern(const char *name)
{
    memset(s_fb, 0xFF, KIBAN_EPD_FB_BYTES);
    if (!strcmp(name, "white")) return true;
    if (!strcmp(name, "black")) { memset(s_fb, 0x00, KIBAN_EPD_FB_BYTES); return true; }
    if (!strcmp(name, "checker")) {   // 40 px squares, black at the origin
        for (int y = 0; y < KIBAN_EPD_HEIGHT; y += 40)
            for (int x = 0; x < KIBAN_EPD_WIDTH; x += 40)
                if (((x / 40) + (y / 40)) % 2 == 0) rect_black(x, y, 40, 40);
        return true;
    }
    if (!strcmp(name, "stripes")) {   // 16 px vertical stripes, then 16 px horizontal stripes in the lower half
        for (int x = 0; x < KIBAN_EPD_WIDTH; x += 32) rect_black(x, 0, 16, KIBAN_EPD_HEIGHT / 2);
        for (int y = KIBAN_EPD_HEIGHT / 2; y < KIBAN_EPD_HEIGHT; y += 32) rect_black(0, y, KIBAN_EPD_WIDTH, 16);
        return true;
    }
    if (!strcmp(name, "marker")) {
        // Filled square at frame (16,16), a bar along the top edge, and an arrowhead pointing to +x.
        // Under any rotation or mirror the square and the arrow direction are distinguishable.
        rect_black(16, 16, 96, 96);
        rect_black(128, 24, 600, 24);
        for (int i = 0; i < 48; i++) rect_black(728 + i, 12 + i / 2, 1, 48 - i);   // triangle, tip at x=775
        rect_black(KIBAN_EPD_WIDTH - 24, KIBAN_EPD_HEIGHT - 24, 8, 8);           // small dot, bottom-right
        return true;
    }
    printf("patterns: white black checker stripes marker\n");
    return false;
}

static void make_gray_ramp(void)
{
    // Four 200 px vertical bands, left to right: black, dark gray, light gray, white.
    for (int y = 0; y < KIBAN_EPD_HEIGHT; y++)
        for (int b = 0; b < KIBAN_EPD_WIDTH / 4; b++) {
            const int x = b * 4;
            const uint8_t v = (uint8_t)(x / 200);   // 0..3
            s_fb2[y * (KIBAN_EPD_WIDTH / 4) + b] = (uint8_t)(v << 6 | v << 4 | v << 2 | v);
        }
}

static void report(const char *what, esp_err_t err)
{
    const kiban_epd_stats_t st = kiban_epd_last_stats();
    if (err == ESP_OK) {
        printf("%s: ok, transfer %lu ms, BUSY %lu ms%s\n", what, (unsigned long)st.transfer_ms,
               (unsigned long)st.busy_ms, st.busy_seen ? "" : " (BUSY never seen high: refresh may not have run)");
        return;
    }
    printf("%s: %s\n", what, esp_err_to_name(err));
    if (err == ESP_ERR_TIMEOUT) {
        // Probe safety rule (design 001 §5): a BUSY timeout powers the panel rail down.
        esp_err_t r = kiban_board_rail_set(KIBAN_RAIL_PANEL_SD, false);
        printf("panel/SD rail off (%s). Recover with: rail panel on, epd init\n", esp_err_to_name(r));
    }
}

static int cmd_epd(int argc, char **argv)
{
    if (argc < 2) goto usage;
    const char *sub = argv[1];
    esp_err_t err;
    if (!strcmp(sub, "init")) {
        err = kiban_epd_init();
        report("init", err);
    } else if (!strcmp(sub, "sleep")) {
        err = kiban_epd_sleep();
        printf("sleep: %s\n", esp_err_to_name(err));
    } else if (!strcmp(sub, "gray")) {
        if (!alloc_fbs()) return 1;
        make_gray_ramp();
        err = kiban_epd_show_gray(s_fb2, 0);
        report("gray (base pass is in the log)", err);
    } else {
        kiban_epd_mode_t mode;
        if (!strcmp(sub, "full")) mode = KIBAN_EPD_MODE_FULL;
        else if (!strcmp(sub, "fast")) mode = KIBAN_EPD_MODE_FAST;
        else if (!strcmp(sub, "partial")) mode = KIBAN_EPD_MODE_PARTIAL;
        else if (!strcmp(sub, "otp")) mode = KIBAN_EPD_MODE_FULL_OTP;
        else goto usage;
        if (argc < 3) goto usage;
        if (!alloc_fbs() || !make_pattern(argv[2])) return 1;
        err = kiban_epd_show(s_fb, mode, 0);
        report(sub, err);
    }
    return err == ESP_OK ? 0 : 1;
usage:
    printf("usage: epd init | epd <full|fast|partial|otp> <white|black|checker|stripes|marker> | epd gray | epd sleep\n");
    return 1;
}

void probe_register_epd_cmds(void)
{
    const esp_console_cmd_t c = {
        .command = "epd",
        .help = "epd init | epd <full|fast|partial|otp> <pattern> | epd gray | epd sleep; prints BUSY ms",
        .func = cmd_epd,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}
