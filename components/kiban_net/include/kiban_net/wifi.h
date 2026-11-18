// Wi-Fi station without NVS: credentials are passed by the caller and kept in RAM (WIFI_STORAGE_RAM).
// Where credentials are stored is product policy. Requires in sdkconfig: CONFIG_ESP_WIFI_NVS_ENABLED=n,
// CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=n. Not thread-safe: one owner task.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *ssid;
    const char *password;
    uint32_t attempt_timeout_ms;   // per attempt (default 15000)
    int attempts;                  // default 5
} kiban_wifi_cfg_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    uint8_t authmode;              // wifi_auth_mode_t
} kiban_wifi_ap_t;

esp_err_t kiban_wifi_init(void);   // netif + default event loop + driver, STA mode, started
esp_err_t kiban_wifi_deinit(void);
// Blocks until an IP is obtained or all attempts fail. PMF capable, WPA3-SAE (hunting-and-pecking and H2E).
esp_err_t kiban_wifi_connect(const kiban_wifi_cfg_t *cfg);
esp_err_t kiban_wifi_disconnect(void);
bool kiban_wifi_connected(void);
esp_err_t kiban_wifi_rssi(int *rssi);
int kiban_wifi_last_reason(void);  // last disconnect reason (wifi_err_reason_t), 0 if none
// Active scan (~2.5 s); fills up to *n entries, strongest first; *n is updated.
esp_err_t kiban_wifi_scan(kiban_wifi_ap_t *aps, uint16_t *n);

#ifdef __cplusplus
}
#endif
