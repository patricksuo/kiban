// Sleep / wake. Wake-source setup follows the vendor firmware (MIT, CloudZao/Metalio-E-INK4 @d12f481
// main/boards/common/power_policy/power_hw.cc:405-420: POWER GPIO3 low-level wake + timer).
#include "kiban/power.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_check.h"
#include "esp_log.h"
#include "kiban/audio.h"
#include "kiban/board.h"
#include "kiban/touch.h"
#include "kiban/epd.h"
#include "kiban/sdcard.h"
#include "kiban/sensors.h"
#include "kiban/ioexp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "kiban.power";

esp_err_t kiban_power_quiesce(void)
{
    kiban_audio_amp(false);
    if (kiban_touch_init() == ESP_OK) kiban_touch_sleep();   // may NACK; not fatal
    return ESP_OK;
}

esp_sleep_wakeup_cause_t kiban_power_light_sleep(const kiban_wake_cfg_t *w)
{
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (w->timer_ms) esp_sleep_enable_timer_wakeup((uint64_t)w->timer_ms * 1000);
    if (w->power_key) {
        gpio_wakeup_enable(KIBAN_PIN_POWER_KEY, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();
    }
    esp_err_t err = esp_light_sleep_start();
    if (w->power_key) gpio_wakeup_disable(KIBAN_PIN_POWER_KEY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "light sleep rejected: %s", esp_err_to_name(err));
        return ESP_SLEEP_WAKEUP_UNDEFINED;
    }
    const uint32_t m = esp_sleep_get_wakeup_causes();   // bit n = esp_sleep_wakeup_cause_t n
    for (int i = 0; i < 32; i++)
        if (m & (1u << i)) return (esp_sleep_wakeup_cause_t)i;
    return ESP_SLEEP_WAKEUP_UNDEFINED;
}

void kiban_power_deep_sleep(const kiban_wake_cfg_t *w)
{
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    if (w->timer_ms) esp_sleep_enable_timer_wakeup((uint64_t)w->timer_ms * 1000);
    if (w->power_key) {
        // GPIO3 has an external pull-up; wake on low.
        esp_sleep_enable_ext1_wakeup_io(1ULL << KIBAN_PIN_POWER_KEY, ESP_EXT1_WAKEUP_ANY_LOW);
    }
    ESP_LOGI(TAG, "deep sleep (timer %lu ms, power key %d)", (unsigned long)w->timer_ms, w->power_key);
    esp_deep_sleep_start();
}

esp_err_t kiban_power_off(uint32_t max_ms)
{
    // Sequence after FreeInk (MIT, metalio-eink4-support.md:102-121) and the vendor PwrOffTask
    // (power_hw.cc:361-385) but bounded: amp off, rails stay on, toggle P1.3 every 100 ms until the
    // power-switch chip cuts power or max_ms elapses (USB keeps the board powered).
    kiban_audio_amp(false);
    ESP_LOGW(TAG, "power off: pulsing PWR_KEY for up to %lu ms", (unsigned long)max_ms);
    bool level = true;
    for (uint32_t t = 0; t < max_ms; t += 100) {
        level = !level;
        kiban_ioexp_set(KIBAN_IOEXP_PWR_KEY_PULSE, level);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    kiban_ioexp_set(KIBAN_IOEXP_PWR_KEY_PULSE, true);   // back to idle high
    ESP_LOGW(TAG, "power off: still running after %lu ms (external power present?)", (unsigned long)max_ms);
    return ESP_ERR_TIMEOUT;
}

esp_err_t kiban_power_prepare(void)
{
    kiban_audio_close();                      // amp off first, then I2S / UART
    if (kiban_epd_ready()) kiban_epd_sleep();
    kiban_sd_unmount();
    gpio_set_level(KIBAN_PIN_MOTOR, 0);
    return kiban_power_quiesce();
}

void kiban_power_off_for(uint32_t seconds)
{
    kiban_power_prepare();
    if (seconds) {
        esp_err_t err = kiban_rtc_wake_after(seconds);
        if (err != ESP_OK) ESP_LOGE(TAG, "RTC wake not armed (%s)", esp_err_to_name(err));
    } else {
        kiban_rtc_wake_cancel();
    }
    kiban_power_off(5000);
    const kiban_wake_cfg_t w = {.timer_ms = seconds ? seconds * 1000 : 0, .power_key = true};
    kiban_power_deep_sleep(&w);
}
