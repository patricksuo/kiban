// P5 commands: sd (read-only) and motor.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "kiban/haptic.h"
#include "kiban/sdcard.h"
#include "probe_cmds.h"

#define MNT "/sd"

static void list_dir(const char *path, int depth, int max_depth)
{
    DIR *d = opendir(path);
    if (!d) { printf("%*s(cannot open %s)\n", depth * 2, "", path); return; }
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != NULL) {
        if (++n > 40) { printf("%*s...\n", depth * 2, ""); break; }
        char full[300];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        struct stat st;
        const bool ok = stat(full, &st) == 0;
        const bool dir = e->d_type == DT_DIR;
        printf("%*s%s%s  %s\n", depth * 2, "", e->d_name, dir ? "/" : "",
               dir ? "" : (ok ? ({ static char b[24]; snprintf(b, sizeof b, "%ld B", (long)st.st_size); b; }) : "?"));
        if (dir && depth + 1 < max_depth) list_dir(full, depth + 1, max_depth);
    }
    closedir(d);
}

static int cmd_sd(int argc, char **argv)
{
    if (argc < 2) { printf("usage: sd mount [khz] | info | ls [path] [depth] | bench <path> | umount\n"); return 1; }
    if (!strcmp(argv[1], "mount")) {
        const int khz = argc > 2 ? atoi(argv[2]) : 0;
        const int64_t t0 = esp_timer_get_time();
        esp_err_t err = kiban_sd_mount(MNT, khz);
        printf("mount: %s (%lld ms)\n", esp_err_to_name(err), (long long)((esp_timer_get_time() - t0) / 1000));
        if (err == ESP_OK) sdmmc_card_print_info(stdout, (sdmmc_card_t *)kiban_sd_card());
        return err == ESP_OK ? 0 : 1;
    }
    if (!kiban_sd_card()) { printf("not mounted\n"); return 1; }
    if (!strcmp(argv[1], "info")) { sdmmc_card_print_info(stdout, (sdmmc_card_t *)kiban_sd_card()); return 0; }
    if (!strcmp(argv[1], "ls")) {
        char path[200];
        snprintf(path, sizeof path, MNT "%s%s", argc > 2 && argv[2][0] != '/' ? "/" : "", argc > 2 ? argv[2] : "");
        list_dir(path, 0, argc > 3 ? atoi(argv[3]) : 1);
        return 0;
    }
    if (!strcmp(argv[1], "bench") && argc > 2) {
        char path[200];
        snprintf(path, sizeof path, MNT "%s%s", argv[2][0] != '/' ? "/" : "", argv[2]);
        FILE *f = fopen(path, "rb");
        if (!f) { printf("cannot open %s\n", path); return 1; }
        const size_t chunk = 32 * 1024;
        uint8_t *buf = heap_caps_malloc(chunk, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!buf) { fclose(f); printf("no memory\n"); return 1; }
        size_t total = 0, r;
        const int64_t t0 = esp_timer_get_time();
        while (total < 8 * 1024 * 1024 && (r = fread(buf, 1, chunk, f)) > 0) total += r;
        const int64_t us = esp_timer_get_time() - t0;
        fclose(f);
        free(buf);
        printf("read %u bytes in %lld ms = %.2f MB/s\n", (unsigned)total, (long long)(us / 1000),
               us ? (double)total / (double)us : 0.0);
        return 0;
    }
    if (!strcmp(argv[1], "hex") && argc > 2) {
        // Read-only dump for copying a file off the card: one line per 64 bytes, "HEX <offset> <hex>".
        char path[200];
        snprintf(path, sizeof path, MNT "%s%s", argv[2][0] != '/' ? "/" : "", argv[2]);
        FILE *f = fopen(path, "rb");
        if (!f) { printf("cannot open %s\n", path); return 1; }
        uint8_t b[64];
        size_t r, off = 0;
        while ((r = fread(b, 1, sizeof b, f)) > 0) {
            printf("HEX %08x ", (unsigned)off);
            for (size_t i = 0; i < r; i++) printf("%02x", b[i]);
            printf("\n");
            off += r;
        }
        fclose(f);
        printf("HEXEND %u\n", (unsigned)off);
        return 0;
    }
    if (!strcmp(argv[1], "umount")) { printf("umount: %s\n", esp_err_to_name(kiban_sd_unmount())); return 0; }
    printf("unknown subcommand\n");
    return 1;
}

static int cmd_motor(int argc, char **argv)
{
    const int ms = argc > 1 ? atoi(argv[1]) : 35;
    printf("motor pulse %d ms\n", ms);
    kiban_haptic_pulse((uint32_t)ms);
    return 0;
}

void probe_register_storage_cmds(void)
{
    const esp_console_cmd_t cmds[] = {
        {.command = "sd", .help = "sd mount [khz] | info | ls [path] [depth] | bench <path> | hex <path> | umount (read-only)", .func = cmd_sd},
        {.command = "motor", .help = "motor [ms]: vibration pulse (default 35 ms, max 1000)", .func = cmd_motor},
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
}
