// Partition table v1 checks: product NVS, assets LittleFS, OTA slot switch.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "probe_cmds.h"

// Product NVS: the partition labelled "nvs" in kiban_16mb_v1.csv (NOT the stock one, now "vendor_nvs").
static int store_nvs(void)
{
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
    if (!p || p->address != 0x10000) { printf("product nvs partition not found at 0x10000\n"); return 1; }
    esp_err_t err = nvs_flash_init_partition("nvs");
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        printf("first use: erasing the product nvs partition only (0x%lx, %lu KB)\n", (unsigned long)p->address,
               (unsigned long)(p->size / 1024));
        ESP_ERROR_CHECK(nvs_flash_erase_partition("nvs"));
        err = nvs_flash_init_partition("nvs");
    }
    if (err != ESP_OK) { printf("nvs init: %s\n", esp_err_to_name(err)); return 1; }
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open("probe", NVS_READWRITE, &h));
    uint32_t n = 0;
    nvs_get_u32(h, "boots", &n);
    n++;
    ESP_ERROR_CHECK(nvs_set_u32(h, "boots", n));
    ESP_ERROR_CHECK(nvs_commit(h));
    uint32_t back = 0;
    nvs_get_u32(h, "boots", &back);
    nvs_close(h);
    nvs_stats_t st;
    nvs_get_stats("nvs", &st);
    printf("nvs ok: counter %lu (read back %lu); entries used %u / total %u\n", (unsigned long)n, (unsigned long)back,
           (unsigned)st.used_entries, (unsigned)st.total_entries);
    return 0;
}

static int store_fs(void)
{
    const esp_vfs_littlefs_conf_t conf = {.base_path = "/assets", .partition_label = "assets",
                                          .format_if_mount_failed = true};   // this partition only
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { printf("littlefs: %s\n", esp_err_to_name(err)); return 1; }
    FILE *f = fopen("/assets/probe.txt", "a");
    if (!f) { printf("open for append failed\n"); return 1; }
    fprintf(f, "probe write\n");
    fclose(f);
    f = fopen("/assets/probe.txt", "r");
    int lines = 0;
    char line[32];
    while (f && fgets(line, sizeof line, f)) lines++;
    if (f) fclose(f);
    size_t total = 0, used = 0;
    esp_littlefs_info("assets", &total, &used);
    printf("assets ok: probe.txt has %d line(s); %u KB used of %u KB\n", lines, (unsigned)(used / 1024),
           (unsigned)(total / 1024));
    return 0;
}

static int cmd_store(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "nvs")) return store_nvs();
    if (argc > 1 && !strcmp(argv[1], "fs")) return store_fs();
    printf("usage: store nvs | fs\n");
    return 1;
}

// ota state | copy | boot <0|1>: copy the running image into the other slot and switch slots.
static int cmd_ota(int argc, char **argv)
{
    const esp_partition_t *run = esp_ota_get_running_partition(), *boot = esp_ota_get_boot_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);
    if (argc < 2 || !strcmp(argv[1], "state")) {
        printf("running %s @0x%lx (state %d), boot %s\n", run->label, (unsigned long)run->address, (int)st,
               boot ? boot->label : "?");
        return 0;
    }
    if (!strcmp(argv[1], "copy")) {
        const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
        esp_app_desc_t desc;
        esp_ota_get_partition_description(run, &desc);
        esp_ota_handle_t h;
        esp_err_t err = esp_ota_begin(dst, OTA_SIZE_UNKNOWN, &h);
        if (err != ESP_OK) { printf("ota begin: %s\n", esp_err_to_name(err)); return 1; }
        uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL);
        // Copy up to the end of the image: read the run partition in 4 KB blocks until the image size.
        esp_partition_pos_t pos = {.offset = run->address, .size = run->size};
        esp_image_metadata_t meta;
        if (esp_image_get_metadata(&pos, &meta) != ESP_OK) { printf("metadata failed\n"); free(buf); esp_ota_abort(h); return 1; }
        for (uint32_t off = 0; off < meta.image_len && err == ESP_OK; off += 4096) {
            const uint32_t n = meta.image_len - off < 4096 ? meta.image_len - off : 4096;
            err = esp_partition_read(run, off, buf, n);
            if (err == ESP_OK) err = esp_ota_write(h, buf, n);
        }
        free(buf);
        if (err == ESP_OK) err = esp_ota_end(h); else esp_ota_abort(h);
        printf("copied %lu bytes of %s to %s: %s\n", (unsigned long)meta.image_len, desc.version, dst->label,
               esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "boot") && argc > 2) {
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                            atoi(argv[2]) ? ESP_PARTITION_SUBTYPE_APP_OTA_1
                                                                          : ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
        esp_err_t err = p ? esp_ota_set_boot_partition(p) : ESP_ERR_NOT_FOUND;
        printf("boot partition -> %s: %s; restarting\n", p ? p->label : "?", esp_err_to_name(err));
        fflush(stdout);
        if (err == ESP_OK) esp_restart();
        return 1;
    }
    return 1;
}

void probe_register_store_cmds(void)
{
    const esp_console_cmd_t cmds[] = {
        {.command = "store", .help = "store nvs | fs: product NVS and assets LittleFS checks", .func = cmd_store},
        {.command = "ota", .help = "ota state | copy | boot <0|1>", .func = cmd_ota},
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
}
