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
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "probe_cmds.h"

static bool s_up;
static EventGroupHandle_t s_ev;
#define EV_GOT_IP BIT0
#define EV_FAIL   BIT1

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(s_ev, EV_GOT_IP);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        printf("disconnected, reason %d\n", d->reason);
        xEventGroupSetBits(s_ev, EV_FAIL);
    }
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
    if (!s_ev) {
        s_ev = xEventGroupCreate();
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    }
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

}

// wifi join <ssid> <password>: credentials live in RAM only (WIFI_STORAGE_RAM, no NVS); never printed.
static int wifi_join(const char *ssid, const char *pass)
{
    esp_err_t err = wifi_up();
    if (err != ESP_OK) { printf("wifi init: %s\n", esp_err_to_name(err)); return 1; }
    // Look for the target first (the SSID is the user's input; it is printed only here, locally).
    wifi_scan_config_t sc = {.ssid = (uint8_t *)ssid};
    uint16_t n = 8;
    wifi_ap_record_t rec[8];
    if (esp_wifi_scan_start(&sc, true) == ESP_OK && esp_wifi_scan_get_ap_records(&n, rec) == ESP_OK) {
        printf("target scan: %u match(es)\n", n);
        for (int i = 0; i < n; i++)
            printf("  ch%u %d dBm %s%s\n", rec[i].primary, rec[i].rssi, auth_name(rec[i].authmode),
                   rec[i].phy_11n ? " 11n" : "");
    }
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, pass, sizeof wc.sta.password);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    memset(&wc, 0, sizeof wc);
    const int64_t t0 = esp_timer_get_time();
    for (int attempt = 1; attempt <= 5; attempt++) {
        xEventGroupClearBits(s_ev, EV_GOT_IP | EV_FAIL);
        esp_wifi_connect();
        const EventBits_t b = xEventGroupWaitBits(s_ev, EV_GOT_IP | EV_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
        if (b & EV_GOT_IP) {
            wifi_ap_record_t ap;
            esp_wifi_sta_get_ap_info(&ap);
            printf("join: got IP on attempt %d after %lld ms; channel %u, RSSI %d dBm\n", attempt,
                   (long long)((esp_timer_get_time() - t0) / 1000), ap.primary, ap.rssi);
            return 0;
        }
        printf("attempt %d failed\n", attempt);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    printf("join: failed after %lld ms\n", (long long)((esp_timer_get_time() - t0) / 1000));
    return 1;
}

// wifi get <url>: download and report throughput (body discarded).
static int wifi_get(const char *url)
{
    esp_http_client_config_t cfg = {.url = url, .timeout_ms = 15000, .buffer_size = 4096,
                                    .crt_bundle_attach = esp_crt_bundle_attach};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return 1;
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_open(c, 0);
    long long total = 0;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(c);
        static char buf[4096];
        int n;
        while ((n = esp_http_client_read(c, buf, sizeof buf)) > 0) total += n;
    }
    const double s = (double)(esp_timer_get_time() - t0) / 1e6;
    printf("get: %s, status %d, %lld bytes in %.2f s = %.1f KB/s\n", esp_err_to_name(err),
           esp_http_client_get_status_code(c), total, s, s > 0 ? (double)total / 1024.0 / s : 0.0);
    esp_http_client_cleanup(c);
    return err == ESP_OK ? 0 : 1;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "off")) { wifi_down(); printf("wifi off\n"); return 0; }
    if (argc > 3 && !strcmp(argv[1], "join")) return wifi_join(argv[2], argv[3]);
    if (argc > 2 && !strcmp(argv[1], "get")) return wifi_get(argv[2]);
    if (argc > 1 && !strcmp(argv[1], "rssi")) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) { printf("not connected\n"); return 1; }
        printf("rssi %d dBm\n", ap.rssi);
        return 0;
    }
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
    const esp_console_cmd_t cmd = {.command = "wifi", .help = "wifi [off | join <ssid> <pass> | get <url> | rssi]: scan (counts / RSSI only) or connection tests", .func = cmd_wifi};
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
