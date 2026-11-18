// TCA9555 driver. Register sequence follows the vendor firmware (MIT,
// CloudZao/Metalio-E-INK4 main/boards/common/bus/IOExpander.hpp) and FreeInk SDK
// (MIT, libs/hardware/BoardConfig/include/MetalioEInk4Board.h).
#include "kiban/ioexp.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "kiban.ioexp";
static const int XFER_TIMEOUT_MS = 50;

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t s_lock;

esp_err_t kiban_ioexp_read16(uint8_t reg, uint16_t *out)
{
    ESP_RETURN_ON_FALSE(s_dev && out, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    uint8_t buf[2];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_dev, &reg, 1, buf, 2, XFER_TIMEOUT_MS), TAG, "read 0x%02x", reg);
    *out = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    return ESP_OK;
}

esp_err_t kiban_ioexp_write16(uint8_t reg, uint16_t value)
{
    ESP_RETURN_ON_FALSE(s_dev, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    const uint8_t buf[3] = {reg, (uint8_t)value, (uint8_t)(value >> 8)};
    return i2c_master_transmit(s_dev, buf, sizeof buf, XFER_TIMEOUT_MS);
}

esp_err_t kiban_ioexp_set(uint8_t pin, bool high)
{
    ESP_RETURN_ON_FALSE(pin < 16, ESP_ERR_INVALID_ARG, TAG, "pin %u", pin);
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "not initialised");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint16_t v;
    esp_err_t err = kiban_ioexp_read16(KIBAN_IOEXP_REG_OUTPUT, &v);
    if (err == ESP_OK) {
        v = high ? (v | (1u << pin)) : (v & ~(1u << pin));
        err = kiban_ioexp_write16(KIBAN_IOEXP_REG_OUTPUT, v);
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t kiban_ioexp_get_input(uint8_t pin, bool *high)
{
    ESP_RETURN_ON_FALSE(pin < 16 && high, ESP_ERR_INVALID_ARG, TAG, "pin %u", pin);
    uint16_t v;
    ESP_RETURN_ON_ERROR(kiban_ioexp_read16(KIBAN_IOEXP_REG_INPUT, &v), TAG, "input");
    *high = (v >> pin) & 1u;
    return ESP_OK;
}

esp_err_t kiban_ioexp_init(i2c_master_bus_handle_t bus, uint32_t wait_ms)
{
    ESP_RETURN_ON_FALSE(bus, ESP_ERR_INVALID_ARG, TAG, "bus");
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");
    }
    s_bus = bus;
    if (!s_dev) {
        const i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = KIBAN_IOEXP_ADDR,
            .scl_speed_hz = 400000,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), TAG, "add device");
    }
    const int64_t deadline = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    int attempt = 0;
    for (;;) {
        attempt++;
        uint16_t cfg;
        if (kiban_ioexp_read16(KIBAN_IOEXP_REG_CONFIG, &cfg) == ESP_OK) {
            ESP_LOGI(TAG, "TCA9555 answered on attempt %d (config=0x%04x)", attempt, cfg);
            return ESP_OK;
        }
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGE(TAG, "TCA9555 did not answer after %d attempts / %lu ms", attempt, (unsigned long)wait_ms);
            return ESP_ERR_TIMEOUT;
        }
        i2c_master_bus_reset(s_bus);  // clock out a stuck slave (9 pulses + STOP)
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
