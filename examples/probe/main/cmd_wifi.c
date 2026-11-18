// Network commands over kiban_net. Scan output: counts / channel / RSSI only (no SSIDs or BSSIDs).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "kiban_net/http.h"
#include "kiban_net/time.h"
#include "kiban_net/wifi.h"
#include "probe_cmds.h"

static int scan(void)
{
    kiban_wifi_ap_t aps[40];
    uint16_t n = 40;
    esp_err_t err = kiban_wifi_scan(aps, &n);
    if (err != ESP_OK) { printf("scan: %s\n", esp_err_to_name(err)); return 1; }
    int best = -127, worst = 0;
    for (int i = 0; i < n; i++) { if (aps[i].rssi > best) best = aps[i].rssi; if (aps[i].rssi < worst) worst = aps[i].rssi; }
    printf("scan: %u APs; RSSI best %d dBm, worst %d dBm\n", n, n ? best : 0, n ? worst : 0);
    return 0;
}

static esp_err_t null_sink(const uint8_t *d, size_t n, void *ctx) { (void)d; (void)n; (void)ctx; return ESP_OK; }

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2) return scan();
    if (!strcmp(argv[1], "off")) { kiban_wifi_deinit(); printf("wifi off\n"); return 0; }
    if (!strcmp(argv[1], "join") && argc > 3) {   // credentials are never printed
        const kiban_wifi_cfg_t c = {.ssid = argv[2], .password = argv[3]};
        esp_err_t err = kiban_wifi_connect(&c);
        int rssi = 0;
        kiban_wifi_rssi(&rssi);
        printf("join: %s; RSSI %d dBm; last reason %d\n", esp_err_to_name(err), rssi, kiban_wifi_last_reason());
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[1], "get") && argc > 2) {
        const kiban_http_req_t r = {.url = argv[2]};
        kiban_http_result_t res;
        esp_err_t err = kiban_http_get(&r, null_sink, NULL, &res);
        printf("get: %s, status %d, %llu bytes in %lu ms = %.1f KB/s\n", esp_err_to_name(err), res.status,
               (unsigned long long)res.bytes, (unsigned long)res.ms, res.ms ? (double)res.bytes / 1.024 / res.ms : 0.0);
        return 0;
    }
    if (!strcmp(argv[1], "dl") && argc > 3) {   // wifi dl <url> <path on SD, e.g. /sd/test.bin> [resume]
        const kiban_http_dl_opts_t o = {.resume = argc > 4 && !strcmp(argv[4], "resume")};
        kiban_http_result_t res;
        esp_err_t err = kiban_http_download(argv[2], argv[3], &o, &res);
        printf("dl: %s, status %d, %llu bytes (length %lld) in %lu ms\n", esp_err_to_name(err), res.status,
               (unsigned long long)res.bytes, (long long)res.content_length, (unsigned long)res.ms);
        return 0;
    }
    if (!strcmp(argv[1], "time")) {
        printf("time sync: %s\n", esp_err_to_name(kiban_time_sync(NULL, 15000)));
        return 0;
    }
    if (!strcmp(argv[1], "rssi")) {
        int rssi;
        if (kiban_wifi_rssi(&rssi) != ESP_OK) { printf("not connected\n"); return 1; }
        printf("rssi %d dBm\n", rssi);
        return 0;
    }
    printf("usage: wifi [off | join <ssid> <pass> | get <url> | dl <url> <path> [resume] | time | rssi]\n");
    return 1;
}

void probe_register_wifi_cmds(void)
{
    const esp_console_cmd_t cmd = {.command = "wifi", .help = "wifi [off|join|get|dl|time|rssi]: kiban_net tests", .func = cmd_wifi};
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
