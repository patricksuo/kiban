#include "kiban_net/wifi.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "kiban.wifi";
#define EV_GOT_IP BIT0
#define EV_FAIL   BIT1

static EventGroupHandle_t s_ev;
static esp_netif_t *s_netif;
static bool s_up, s_connected;
static int s_reason;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        xEventGroupSetBits(s_ev, EV_GOT_IP);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_reason = ((const wifi_event_sta_disconnected_t *)data)->reason;
        xEventGroupSetBits(s_ev, EV_FAIL);
    }
}

esp_err_t kiban_wifi_init(void)
{
    if (s_up) return ESP_OK;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    if (!s_netif) s_netif = esp_netif_create_default_wifi_sta();
    if (!s_ev) {
        s_ev = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_ev, ESP_ERR_NO_MEM, TAG, "event group");
        ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL), TAG, "ev");
        ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "ev");
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    s_up = true;
    return ESP_OK;
}

esp_err_t kiban_wifi_deinit(void)
{
    if (!s_up) return ESP_OK;
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    s_up = s_connected = false;
    return ESP_OK;
}

esp_err_t kiban_wifi_connect(const kiban_wifi_cfg_t *c)
{
    ESP_RETURN_ON_FALSE(c && c->ssid, ESP_ERR_INVALID_ARG, TAG, "cfg");
    ESP_RETURN_ON_ERROR(kiban_wifi_init(), TAG, "init");
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, c->ssid, sizeof wc.sta.ssid);
    if (c->password) strlcpy((char *)wc.sta.password, c->password, sizeof wc.sta.password);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    memset(&wc, 0, sizeof wc);   // do not keep a second copy of the password
    ESP_RETURN_ON_ERROR(err, TAG, "config");
    const int attempts = c->attempts > 0 ? c->attempts : 5;
    const uint32_t per = c->attempt_timeout_ms ? c->attempt_timeout_ms : 15000;
    for (int i = 1; i <= attempts; i++) {
        xEventGroupClearBits(s_ev, EV_GOT_IP | EV_FAIL);
        esp_wifi_connect();
        const EventBits_t b = xEventGroupWaitBits(s_ev, EV_GOT_IP | EV_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(per));
        if (b & EV_GOT_IP) {
            ESP_LOGI(TAG, "connected on attempt %d", i);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "attempt %d failed (reason %d)", i, s_reason);
        if (!(b & EV_FAIL)) esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t kiban_wifi_disconnect(void) { return s_up ? esp_wifi_disconnect() : ESP_OK; }
bool kiban_wifi_connected(void) { return s_connected; }
int kiban_wifi_last_reason(void) { return s_reason; }

esp_err_t kiban_wifi_rssi(int *rssi)
{
    wifi_ap_record_t ap;
    ESP_RETURN_ON_ERROR(esp_wifi_sta_get_ap_info(&ap), TAG, "not connected");
    *rssi = ap.rssi;
    return ESP_OK;
}

esp_err_t kiban_wifi_scan(kiban_wifi_ap_t *aps, uint16_t *n)
{
    ESP_RETURN_ON_FALSE(aps && n && *n, ESP_ERR_INVALID_ARG, TAG, "args");
    ESP_RETURN_ON_ERROR(kiban_wifi_init(), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_wifi_scan_start(NULL, true), TAG, "scan");
    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    uint16_t want = *n < found ? *n : found;
    wifi_ap_record_t *rec = calloc(want ? want : 1, sizeof *rec);
    ESP_RETURN_ON_FALSE(rec, ESP_ERR_NO_MEM, TAG, "mem");
    esp_err_t err = esp_wifi_scan_get_ap_records(&want, rec);
    for (int i = 0; err == ESP_OK && i < want; i++) {
        strlcpy(aps[i].ssid, (const char *)rec[i].ssid, sizeof aps[i].ssid);
        aps[i].rssi = rec[i].rssi;
        aps[i].channel = rec[i].primary;
        aps[i].authmode = (uint8_t)rec[i].authmode;
    }
    free(rec);
    esp_wifi_clear_ap_list();   // release records not copied out
    *n = err == ESP_OK ? want : 0;
    return err;
}
