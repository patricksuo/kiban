// Vibration motor: plain GPIO pulse, as in the vendor firmware (MIT, CloudZao/Metalio-E-INK4
// @d12f481 main/boards/metalio-e-ink-4/input/metalio_vibe.cc:40-55, pulse 35 ms in config.h:62).
#include "kiban/haptic.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kiban/board.h"

esp_err_t kiban_haptic_pulse(uint32_t ms)
{
    if (ms > 1000) ms = 1000;
    gpio_set_level(KIBAN_PIN_MOTOR, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    gpio_set_level(KIBAN_PIN_MOTOR, 0);
    return ESP_OK;
}
