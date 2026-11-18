// Vibration motor on GPIO44 (plain on/off; vendor short pulse = 35 ms).
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t kiban_haptic_pulse(uint32_t ms);   // blocking; capped at 1000 ms

#ifdef __cplusplus
}
#endif
