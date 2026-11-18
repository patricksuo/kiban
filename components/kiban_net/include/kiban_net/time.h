// SNTP time sync. Sets the system clock and writes the PCF8563 in UTC (clearing its VL flag).
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Requires a network connection. server NULL = "pool.ntp.org".
esp_err_t kiban_time_sync(const char *server, uint32_t timeout_ms);
// Set the system clock from the PCF8563 (UTC) at boot; ESP_ERR_INVALID_STATE if the RTC time is not valid.
esp_err_t kiban_time_from_rtc(void);

#ifdef __cplusplus
}
#endif
