// Wi-Fi RF check: scan and print counts / channels / RSSI only (no SSIDs or BSSIDs: the repo is public
// and scan results identify the location). Wi-Fi NVS and PHY calibration storage are disabled in
// sdkconfig.defaults, so the stock NVS partition is not touched.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "probe_cmds.h"

static bool s_up;

static esp_err_t wifi_up(void)
{
    if (s_up) return ESP_OK;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    static bool netif_made;
    if (!netif_made) { esp_netif_create_default_wifi_sta(); netif_made = true; }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;
    if ((err = esp_wifi_init(&cfg)) != ESP_OK) return err;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return err;
    if ((err = esp_wifi_start()) != ESP_OK) return err;
    s_up = true;
    return ESP_OK;
}

static void wifi_down(void)
{
    if (!s_up) return;
    esp_wifi_stop();
    esp_wifi_deinit();
    s_up = false;
}

static const char *auth_name(wifi_auth_mode_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "wep";
    case WIFI_AUTH_WPA_PSK: return "wpa";
    case WIFI_AUTH_WPA2_PSK: return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "wpa/wpa2";
    case WIFI_AUTH_WPA3_PSK: return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2/wpa3";
    default: return "other";
    }
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "off")) { wifi_down(); printf("wifi off\n"); return 0; }
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = wifi_up();
    printf("wifi init: %s (%lld ms)  free internal heap %u\n", esp_err_to_name(err),
           (long long)((esp_timer_get_time() - t0) / 1000), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (err != ESP_OK) return 1;
    const int64_t t1 = esp_timer_get_time();
    err = esp_wifi_scan_start(NULL, true);
    const int64_t scan_ms = (esp_timer_get_time() - t1) / 1000;
    if (err != ESP_OK) { printf("scan: %s\n", esp_err_to_name(err)); return 1; }
    uint16_t n = 40;
    wifi_ap_record_t *rec = calloc(n, sizeof *rec);
    if (!rec) return 1;
    esp_wifi_scan_get_ap_records(&n, rec);
    int best = -127, worst = 0, ch_count[15] = {0};
    for (int i = 0; i < n; i++) {
        if (rec[i].rssi > best) best = rec[i].rssi;
        if (rec[i].rssi < worst) worst = rec[i].rssi;
        if (rec[i].primary < 15) ch_count[rec[i].primary]++;
    }
    printf("scan: %u APs in %lld ms; RSSI best %d dBm, worst %d dBm\n", n, (long long)scan_ms, n ? best : 0, n ? worst : 0);
    printf("per channel:");
    for (int c = 1; c < 15; c++) if (ch_count[c]) printf(" ch%d=%d", c, ch_count[c]);
    printf("\n");
    for (int i = 0; i < n && i < 10; i++)   // strongest first (scan results are sorted by RSSI)
        printf("  #%d ch%-2u %4d dBm %s\n", i + 1, rec[i].primary, rec[i].rssi, auth_name(rec[i].authmode));
    free(rec);
    return 0;
}

void probe_register_wifi_cmds(void)
{
    const esp_console_cmd_t cmd = {.command = "wifi", .help = "wifi [off]: scan (counts / channel / RSSI only)", .func = cmd_wifi};
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
