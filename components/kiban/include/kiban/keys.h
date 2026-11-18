// Debounced key events for every input key on the Metalio E-Ink 4. Polling only: TCA9555 P1.2 toggles on
// its own and pulses the expander INT (GPIO2), so GPIO2 is unusable as an interrupt or wake source
// [measured] research/003 P7. Call kiban_keys_poll() every 10-20 ms from one task.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KIBAN_KEY_POWER = 0,   // GPIO3, external pull-up, active low
    KIBAN_KEY_AI,          // GPIO0 (BOOT strap): internal pull-up, active low, never driven
    KIBAN_KEY_VOL_UP,      // TCA9555 P1.0, active low
    KIBAN_KEY_VOL_DOWN,    // TCA9555 P0.7, active low
    KIBAN_KEY_COVER_LEFT,  // touch (80,900)
    KIBAN_KEY_COVER_MIDDLE,// touch (400,900)
    KIBAN_KEY_COVER_RIGHT, // touch (240,900)
    KIBAN_KEY_COUNT
} kiban_key_t;

typedef enum { KIBAN_KEY_PRESS, KIBAN_KEY_LONG, KIBAN_KEY_RELEASE } kiban_key_action_t;

typedef struct {
    kiban_key_t key;
    kiban_key_action_t action;
    bool after_long;      // RELEASE that follows a LONG
    int64_t t_us;
} kiban_key_event_t;

typedef struct {
    uint32_t debounce_ms;  // default 20
    uint32_t long_ms;      // default 600
    bool cover_keys;       // read the touch controller for the cover keys (default true)
} kiban_keys_cfg_t;

#define KIBAN_KEYS_CFG_DEFAULT() { .debounce_ms = 20, .long_ms = 600, .cover_keys = true }

// Requires kiban_board_init(). Configures GPIO0/GPIO3 inputs (GPIO0 internal pull-up) and the touch driver.
esp_err_t kiban_keys_init(const kiban_keys_cfg_t *cfg);
// Sample all keys once; writes up to max events, returns how many.
size_t kiban_keys_poll(int64_t now_us, kiban_key_event_t *ev, size_t max);
// Current debounced state of one key.
bool kiban_keys_down(kiban_key_t key);

#ifdef __cplusplus
}
#endif
