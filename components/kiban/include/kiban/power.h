// Sleep and wake. ESP32 deep sleep does NOT power the board down: the main and panel/SD rails,
// the modem and the BT audio module stay powered [community] FreeInk metalio-eink4-support.md:117-121.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_sleep.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t timer_ms;      // 0 = no timer wake
    bool power_key;         // wake on POWER (GPIO3, active low)
} kiban_wake_cfg_t;

// Amplifier off and touch controller to sleep. The caller must put the panel to sleep
// (kiban_epd_sleep) and close files / unmount the SD card first.
esp_err_t kiban_power_quiesce(void);

// Light sleep: RAM and peripherals keep state; returns the wake cause. USB-Serial/JTAG may drop.
esp_sleep_wakeup_cause_t kiban_power_light_sleep(const kiban_wake_cfg_t *wake);

// Deep sleep: wakes through a reset. Does not return.
void kiban_power_deep_sleep(const kiban_wake_cfg_t *wake) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
