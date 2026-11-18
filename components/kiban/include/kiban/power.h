// Sleep, wake and power-off. ESP32 deep sleep does NOT power the board down: the main and panel/SD
// rails, the modem and the BT audio module stay powered [community] FreeInk metalio-eink4-support.md:117-121.
// kiban_power_off() cuts power within ~2 s even with USB attached, and the PCF8563 INT line can power
// the board back on (kiban_rtc_wake_after() before power-off) [measured] research/003 P7.
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

// Amplifier off and touch controller to sleep (low level; prefer kiban_power_prepare()).
esp_err_t kiban_power_quiesce(void);

// Close everything kiban owns before sleep / power-off: audio (amp off, I2S/UART released), panel to deep
// sleep if initialised, touch to sleep, SD card unmounted, motor off. The caller closes its own files first.
esp_err_t kiban_power_prepare(void);

// Light sleep: RAM and peripherals keep state; returns the wake cause. USB-Serial/JTAG may drop.
esp_sleep_wakeup_cause_t kiban_power_light_sleep(const kiban_wake_cfg_t *wake);

// Hardware power-off via the power-switch chip. On battery it does not return. Returns
// ESP_ERR_TIMEOUT after max_ms when external power keeps the board on (caller then falls back,
// e.g. to deep sleep with POWER wake). Call kiban_power_quiesce() and put the panel to sleep first.
esp_err_t kiban_power_off(uint32_t max_ms);

// Power off and come back after `seconds` (0 = stay off until POWER): kiban_power_prepare(), arm the RTC
// (1 Hz up to 255 s, 1/60 Hz up to 255 min), pulse the power-switch chip. If power stays on (it did not on
// this unit, even with USB [measured]), falls back to deep sleep with the same timer and POWER wake.
// Does not return.
void kiban_power_off_for(uint32_t seconds) __attribute__((noreturn));

// Deep sleep: wakes through a reset. Does not return.
void kiban_power_deep_sleep(const kiban_wake_cfg_t *wake) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
