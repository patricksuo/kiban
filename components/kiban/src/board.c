// Board bring-up. Boot levels mirror the vendor firmware (MIT,
// CloudZao/Metalio-E-INK4 metalio_io_expander.cc) and the safe ordering of
// FreeInk SDK (MIT, MetalioEInk4Board.h ensureBooted()).
#include "kiban/board.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kiban/ioexp.h"
#include "kiban/sensors.h"

static const char *TAG = "kiban.board";
static i2c_master_bus_handle_t s_bus;

#define PIN_BIT(n) ((uint16_t)1u << (n))

static const uint16_t OUTPUTS = PIN_BIT(KIBAN_IOEXP_USB_MUX_SEL) | PIN_BIT(KIBAN_IOEXP_PA_SWITCH) |
                                PIN_BIT(KIBAN_IOEXP_PA_EN) | PIN_BIT(KIBAN_IOEXP_PANEL_SD_PWR) |
                                PIN_BIT(KIBAN_IOEXP_MAIN_PWR) | PIN_BIT(KIBAN_IOEXP_TOUCH_RST) |
                                PIN_BIT(KIBAN_IOEXP_PWR_KEY_PULSE);

i2c_master_bus_handle_t kiban_board_i2c(void) { return s_bus; }

esp_err_t kiban_board_rail_set(kiban_rail_t rail, bool on)
{
    switch (rail) {
    case KIBAN_RAIL_MAIN:     return kiban_ioexp_set(KIBAN_IOEXP_MAIN_PWR, on);
    case KIBAN_RAIL_PANEL_SD: return kiban_ioexp_set(KIBAN_IOEXP_PANEL_SD_PWR, on);
    default:                  return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t kiban_board_init(const kiban_board_cfg_t *cfg)
{
    const kiban_board_cfg_t def = KIBAN_BOARD_CFG_DEFAULT();
    if (!cfg) cfg = &def;

    // Motor off first: it is a plain GPIO and independent of the expander.
    const gpio_config_t motor = {.pin_bit_mask = 1ULL << KIBAN_PIN_MOTOR, .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_set_level(KIBAN_PIN_MOTOR, 0), TAG, "motor level");
    ESP_RETURN_ON_ERROR(gpio_config(&motor), TAG, "motor gpio");

    if (!s_bus) {
        const i2c_master_bus_config_t bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = KIBAN_PIN_I2C_SDA,
            .scl_io_num = KIBAN_PIN_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus");
    }
    ESP_RETURN_ON_ERROR(kiban_ioexp_init(s_bus, cfg->expander_wait_ms), TAG, "expander");

    uint16_t latch, conf;
    ESP_RETURN_ON_ERROR(kiban_ioexp_read16(KIBAN_IOEXP_REG_OUTPUT, &latch), TAG, "latch");
    ESP_RETURN_ON_ERROR(kiban_ioexp_read16(KIBAN_IOEXP_REG_CONFIG, &conf), TAG, "config");
    ESP_LOGI(TAG, "expander before init: output=0x%04x config=0x%04x", latch, conf);

    // 1) Levels first. USB mux -> ESP, PA switch -> ESP, amp off, main rail on,
    //    power-off pulse line idle high, touch in reset, panel/SD rail off until step 3.
    latch &= ~OUTPUTS;
    latch |= PIN_BIT(KIBAN_IOEXP_USB_MUX_SEL) | PIN_BIT(KIBAN_IOEXP_MAIN_PWR) | PIN_BIT(KIBAN_IOEXP_PWR_KEY_PULSE);
    ESP_RETURN_ON_ERROR(kiban_ioexp_write16(KIBAN_IOEXP_REG_OUTPUT, latch), TAG, "write latch");
    // 2) Then directions: OUTPUTS become outputs, everything else stays input. No inversion.
    ESP_RETURN_ON_ERROR(kiban_ioexp_write16(KIBAN_IOEXP_REG_CONFIG, (uint16_t)~OUTPUTS), TAG, "write config");
    ESP_RETURN_ON_ERROR(kiban_ioexp_write16(KIBAN_IOEXP_REG_POLARITY, 0), TAG, "write polarity");

    // 3) Panel/SD rail on, release touch reset, settle.
    ESP_RETURN_ON_ERROR(kiban_board_rail_set(KIBAN_RAIL_PANEL_SD, true), TAG, "panel rail");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(kiban_ioexp_set(KIBAN_IOEXP_TOUCH_RST, true), TAG, "touch reset");
    vTaskDelay(pdMS_TO_TICKS(120));

    // Expander INT is open-drain from the TCA9555; keep a pull-up.
    const gpio_config_t intr = {.pin_bit_mask = 1ULL << KIBAN_PIN_IOEXP_INT, .mode = GPIO_MODE_INPUT,
                                .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&intr), TAG, "ioexp int");

    // Charging safety: enforce the termination voltage on every boot (another firmware or a charger
    // reset may have left a higher value).
    // Not fatal: the rest of the board is usable, but say so loudly. [measured] 2026-11-18: writes to
    // 0x04/0x05 are ignored on this device, even after the vendor unlock; VREG stays 4400 mV.
    if (cfg->charge_vreg_mv) {
        esp_err_t verr = kiban_charger_set_vreg_mv(cfg->charge_vreg_mv);
        if (verr != ESP_OK) ESP_LOGE(TAG, "charger VREG NOT enforced (%s); charging still uses the previous VREG",
                                     esp_err_to_name(verr));
    }

    ESP_LOGI(TAG, "board up");
    return ESP_OK;
}
