// CST816S driver. Register layout and reset/sleep sequence follow the vendor firmware (MIT,
// CloudZao/Metalio-E-INK4 @d12f481 main/boards/metalio-e-ink-4/display/metalio_touch.c) and the
// esp_lcd_touch_cst816s component it uses.
#include "kiban/touch.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kiban/board.h"
#include "kiban/ioexp.h"

static const char *TAG = "kiban.touch";
static i2c_master_dev_handle_t s_dev;

#define REG_GESTURE 0x01   // then: fingers, XH, XL, YH, YL
#define REG_SLEEP   0xA5
#define XFER_TIMEOUT_MS 20

esp_err_t kiban_touch_init(void)
{
    i2c_master_bus_handle_t bus = kiban_board_i2c();
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_STATE, TAG, "board not initialised");
    if (!s_dev) {
        const i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = KIBAN_TOUCH_ADDR,
            .scl_speed_hz = 400000,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add device");
    }
    const gpio_config_t intr = {.pin_bit_mask = 1ULL << KIBAN_PIN_TOUCH_INT, .mode = GPIO_MODE_INPUT,
                                .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&intr), TAG, "int gpio");
    return ESP_OK;
}

esp_err_t kiban_touch_read(kiban_touch_point_t *out)
{
    ESP_RETURN_ON_FALSE(s_dev && out, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    const uint8_t reg = REG_GESTURE;
    uint8_t b[6];
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, b, sizeof b, XFER_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    out->gesture = b[0];
    out->fingers = b[1] & 0x0F;
    out->event = (kiban_touch_event_t)(b[2] >> 6);
    out->x = (uint16_t)(((b[2] & 0x0F) << 8) | b[3]);
    out->y = (uint16_t)(((b[4] & 0x0F) << 8) | b[5]);
    return ESP_OK;
}

bool kiban_touch_int_active(void) { return gpio_get_level(KIBAN_PIN_TOUCH_INT) == 0; }

esp_err_t kiban_touch_sleep(void)
{
    ESP_RETURN_ON_FALSE(s_dev, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    const uint8_t buf[2] = {REG_SLEEP, 0x03};
    return i2c_master_transmit(s_dev, buf, sizeof buf, XFER_TIMEOUT_MS);
}

esp_err_t kiban_touch_reset(void)
{
    ESP_RETURN_ON_ERROR(kiban_ioexp_set(KIBAN_IOEXP_TOUCH_RST, false), TAG, "rst low");
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_RETURN_ON_ERROR(kiban_ioexp_set(KIBAN_IOEXP_TOUCH_RST, true), TAG, "rst high");
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

kiban_cover_key_t kiban_touch_cover_key(uint16_t x, uint16_t y)
{
    if (y < 850 || y > 950) return KIBAN_COVER_NONE;
    if (x == 80) return KIBAN_COVER_LEFT;
    if (x == 400) return KIBAN_COVER_MIDDLE;
    if (x == 240) return KIBAN_COVER_RIGHT;
    return KIBAN_COVER_NONE;
}
