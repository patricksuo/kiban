#include "i2c_dev.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "kiban/board.h"

static const char *TAG = "kiban.i2c";
#define MAX_DEVS 8

static struct { uint8_t addr; i2c_master_dev_handle_t h; } s_devs[MAX_DEVS];
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static i2c_master_dev_handle_t dev_for(uint8_t addr)
{
    if (!s_lock) {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        portENTER_CRITICAL(&s_init_mux);
        if (!s_lock) { s_lock = m; m = NULL; }
        portEXIT_CRITICAL(&s_init_mux);
        if (m) vSemaphoreDelete(m);
    }
    i2c_master_dev_handle_t found = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_DEVS && !found; i++)
        if (s_devs[i].h && s_devs[i].addr == addr) found = s_devs[i].h;
    i2c_master_bus_handle_t bus = kiban_board_i2c();
    for (int i = 0; i < MAX_DEVS && !found && bus; i++) {
        if (s_devs[i].h) continue;
        const i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr,
                                         .scl_speed_hz = kiban_board_i2c_hz()};
        if (i2c_master_bus_add_device(bus, &cfg, &s_devs[i].h) == ESP_OK) {
            s_devs[i].addr = addr;
            found = s_devs[i].h;
        }
        break;
    }
    xSemaphoreGive(s_lock);
    return found;
}

esp_err_t kiban_i2c_rd(uint8_t addr, uint8_t reg, uint8_t *buf, size_t n)
{
    i2c_master_dev_handle_t h = dev_for(addr);
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_STATE, TAG, "no device 0x%02x", addr);
    return i2c_master_transmit_receive(h, &reg, 1, buf, n, KIBAN_I2C_XFER_TIMEOUT_MS);
}

esp_err_t kiban_i2c_wr(uint8_t addr, uint8_t reg, uint8_t val)
{
    i2c_master_dev_handle_t h = dev_for(addr);
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_STATE, TAG, "no device 0x%02x", addr);
    const uint8_t b[2] = {reg, val};
    return i2c_master_transmit(h, b, 2, KIBAN_I2C_XFER_TIMEOUT_MS);
}

esp_err_t kiban_i2c_wr16(uint8_t addr, uint8_t reg, uint16_t val)
{
    i2c_master_dev_handle_t h = dev_for(addr);
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_STATE, TAG, "no device 0x%02x", addr);
    const uint8_t b[3] = {reg, (uint8_t)val, (uint8_t)(val >> 8)};
    return i2c_master_transmit(h, b, 3, KIBAN_I2C_XFER_TIMEOUT_MS);
}
