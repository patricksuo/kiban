#include "kiban_net/time.h"

#include <sys/time.h>
#include <time.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "kiban/sensors.h"

static const char *TAG = "kiban.time";

esp_err_t kiban_time_sync(const char *server, uint32_t timeout_ms)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(server ? server : "pool.ntp.org");
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&cfg), TAG, "sntp init");
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms ? timeout_ms : 15000));
    esp_netif_sntp_deinit();
    ESP_RETURN_ON_ERROR(err, TAG, "sntp sync");
    const time_t now = time(NULL);
    struct tm u;
    gmtime_r(&now, &u);
    const kiban_rtc_time_t t = {.year = u.tm_year + 1900, .month = u.tm_mon + 1, .day = u.tm_mday, .weekday = u.tm_wday,
                                .hour = u.tm_hour, .minute = u.tm_min, .second = u.tm_sec};
    ESP_RETURN_ON_ERROR(kiban_rtc_set(&t), TAG, "rtc set");
    ESP_LOGI(TAG, "time synced: %04d-%02d-%02d %02d:%02d:%02d UTC", t.year, t.month, t.day, t.hour, t.minute, t.second);
    return ESP_OK;
}

esp_err_t kiban_time_from_rtc(void)
{
    kiban_rtc_time_t t;
    ESP_RETURN_ON_ERROR(kiban_rtc_get(&t), TAG, "rtc get");
    ESP_RETURN_ON_FALSE(!t.integrity_lost && t.year >= 2024 && t.year <= 2099, ESP_ERR_INVALID_STATE, TAG,
                        "RTC time not valid");
    struct tm u = {.tm_year = t.year - 1900, .tm_mon = t.month - 1, .tm_mday = t.day, .tm_hour = t.hour,
                   .tm_min = t.minute, .tm_sec = t.second};
    setenv("TZ", "UTC0", 1);
    tzset();
    const struct timeval tv = {.tv_sec = mktime(&u)};
    return settimeofday(&tv, NULL) == 0 ? ESP_OK : ESP_FAIL;
}
