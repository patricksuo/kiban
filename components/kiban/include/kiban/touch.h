// CST816S capacitive touch controller at 0x15 (Metalio E-Ink 4).
// Native coordinates are portrait 480x800; the cover's three virtual keys report y beyond the panel
// (vendor: HOME (80,900), NEXT (240,900), PREV (400,900)).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KIBAN_TOUCH_ADDR 0x15

typedef enum {
    KIBAN_TOUCH_EV_DOWN = 0,
    KIBAN_TOUCH_EV_UP = 1,
    KIBAN_TOUCH_EV_CONTACT = 2,
    KIBAN_TOUCH_EV_NONE = 3,
} kiban_touch_event_t;

typedef struct {
    uint8_t fingers;        // 0 or 1
    uint8_t gesture;        // controller gesture code (0 = none)
    kiban_touch_event_t event;
    uint16_t x;             // native portrait, 0..479 on the panel
    uint16_t y;             // native portrait, 0..799 on the panel; larger = cover key area
} kiban_touch_point_t;

// Cover keys (the printed areas below the panel), named by physical position. Each reports a fixed
// coordinate while held [measured] research/003 P3: LEFT (80,900), MIDDLE (400,900), RIGHT (240,900);
// the vendor source comments swap MIDDLE and RIGHT. Their meaning (home / previous / next) is product policy.
typedef enum { KIBAN_COVER_NONE = 0, KIBAN_COVER_LEFT, KIBAN_COVER_MIDDLE, KIBAN_COVER_RIGHT } kiban_cover_key_t;
kiban_cover_key_t kiban_touch_cover_key(uint16_t x, uint16_t y);

// Finger taps land about +15..+22 px below the aimed point (y) [measured]; not corrected here.

// Requires kiban_board_init() (I2C bus up, touch reset released). Does not read a chip ID:
// the CST816S may NACK while nothing touches it.
esp_err_t kiban_touch_init(void);
// One read of the report registers. ESP_ERR_INVALID_RESPONSE / timeout while idle is normal (NACK).
esp_err_t kiban_touch_read(kiban_touch_point_t *out);
// INT line level (GPIO1, active low while a report is pending).
bool kiban_touch_int_active(void);
esp_err_t kiban_touch_sleep(void);   // 0xA5 = 0x03; wake needs kiban_touch_reset()
esp_err_t kiban_touch_reset(void);   // reset pulse via TCA9555 P1.1, then 120 ms settle

#ifdef __cplusplus
}
#endif
