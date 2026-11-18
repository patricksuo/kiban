#include "kiban/keys.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "kiban/board.h"
#include "kiban/ioexp.h"
#include "kiban/touch.h"

static const char *TAG = "kiban.keys";

typedef struct {
    bool raw, stable, long_fired;
    int64_t raw_since, down_since;
} key_state_t;

static key_state_t s_k[KIBAN_KEY_COUNT];
static kiban_keys_cfg_t s_cfg;
static bool s_init;

esp_err_t kiban_keys_init(const kiban_keys_cfg_t *cfg)
{
    const kiban_keys_cfg_t d = KIBAN_KEYS_CFG_DEFAULT();
    s_cfg = cfg ? *cfg : d;
    if (!s_cfg.debounce_ms) s_cfg.debounce_ms = d.debounce_ms;
    if (!s_cfg.long_ms) s_cfg.long_ms = d.long_ms;
    const gpio_config_t io = {.pin_bit_mask = (1ULL << KIBAN_PIN_BOOT_KEY) | (1ULL << KIBAN_PIN_POWER_KEY),
                              .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "gpio");   // GPIO0 has no external pull-up [measured]
    if (s_cfg.cover_keys) ESP_RETURN_ON_ERROR(kiban_touch_init(), TAG, "touch");
    for (int i = 0; i < KIBAN_KEY_COUNT; i++) s_k[i] = (key_state_t){0};
    s_init = true;
    return ESP_OK;
}

static void sample(bool raw[KIBAN_KEY_COUNT])
{
    for (int i = 0; i < KIBAN_KEY_COUNT; i++) raw[i] = false;
    raw[KIBAN_KEY_POWER] = gpio_get_level(KIBAN_PIN_POWER_KEY) == 0;
    raw[KIBAN_KEY_AI] = gpio_get_level(KIBAN_PIN_BOOT_KEY) == 0;
    uint16_t in;
    if (kiban_ioexp_read16(KIBAN_IOEXP_REG_INPUT, &in) == ESP_OK) {
        raw[KIBAN_KEY_VOL_UP] = !((in >> KIBAN_IOEXP_VOL_UP) & 1);
        raw[KIBAN_KEY_VOL_DOWN] = !((in >> KIBAN_IOEXP_VOL_DOWN) & 1);
    } else {   // keep the previous raw state on an isolated read failure
        raw[KIBAN_KEY_VOL_UP] = s_k[KIBAN_KEY_VOL_UP].raw;
        raw[KIBAN_KEY_VOL_DOWN] = s_k[KIBAN_KEY_VOL_DOWN].raw;
    }
    if (s_cfg.cover_keys) {
        kiban_touch_point_t p;
        if (kiban_touch_read(&p) == ESP_OK && p.fingers) {   // NACK while untouched = no key
            switch (kiban_touch_cover_key(p.x, p.y)) {
            case KIBAN_COVER_LEFT: raw[KIBAN_KEY_COVER_LEFT] = true; break;
            case KIBAN_COVER_MIDDLE: raw[KIBAN_KEY_COVER_MIDDLE] = true; break;
            case KIBAN_COVER_RIGHT: raw[KIBAN_KEY_COVER_RIGHT] = true; break;
            default: break;
            }
        }
    }
}

size_t kiban_keys_poll(int64_t now, kiban_key_event_t *ev, size_t max)
{
    if (!s_init) return 0;
    bool raw[KIBAN_KEY_COUNT];
    sample(raw);
    size_t n = 0;
    const int64_t deb = (int64_t)s_cfg.debounce_ms * 1000, lng = (int64_t)s_cfg.long_ms * 1000;
    for (int i = 0; i < KIBAN_KEY_COUNT; i++) {
        key_state_t *k = &s_k[i];
        if (raw[i] != k->raw) { k->raw = raw[i]; k->raw_since = now; }
        if (k->raw != k->stable && now - k->raw_since >= deb) {
            k->stable = k->raw;
            if (k->stable) {
                k->down_since = now;
                k->long_fired = false;
                if (n < max) ev[n++] = (kiban_key_event_t){.key = (kiban_key_t)i, .action = KIBAN_KEY_PRESS, .t_us = now};
            } else if (n < max) {
                ev[n++] = (kiban_key_event_t){.key = (kiban_key_t)i, .action = KIBAN_KEY_RELEASE,
                                              .after_long = k->long_fired, .t_us = now};
            }
        }
        if (k->stable && !k->long_fired && now - k->down_since >= lng) {
            k->long_fired = true;
            if (n < max) ev[n++] = (kiban_key_event_t){.key = (kiban_key_t)i, .action = KIBAN_KEY_LONG, .t_us = now};
        }
    }
    return n;
}

bool kiban_keys_down(kiban_key_t key) { return key < KIBAN_KEY_COUNT && s_k[key].stable; }
