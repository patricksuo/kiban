// SD card mount. Pins and DAT3 handling from the vendor firmware (MIT, CloudZao/Metalio-E-INK4
// @d12f481 main/boards/metalio-e-ink-4/config.h:29-33) and FreeInk SDK (MIT, MetalioEInk4Board.h:20).
#include "kiban/sdcard.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

static const char *TAG = "kiban.sd";
static sdmmc_card_t *s_card;
static char s_mount[16];

esp_err_t kiban_sd_mount(const char *mount_point, int max_freq_khz)
{
    if (s_card) return ESP_OK;
    // DAT3 must read high or the card may enter SPI mode; GPIO46 is input-only.
    const gpio_config_t dat3 = {.pin_bit_mask = 1ULL << KIBAN_SD_PIN_DAT3, .mode = GPIO_MODE_INPUT,
                                .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&dat3), TAG, "dat3");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = max_freq_khz > 0 ? max_freq_khz : SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = KIBAN_SD_PIN_CLK;
    slot.cmd = KIBAN_SD_PIN_CMD;
    slot.d0 = KIBAN_SD_PIN_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    const esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,   // never format
        .max_files = 4,
        .allocation_unit_size = 0,
    };
    strncpy(s_mount, mount_point, sizeof s_mount - 1);
    esp_err_t err = esp_vfs_fat_sdmmc_mount(s_mount, &host, &slot, &mcfg, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t kiban_sd_unmount(void)
{
    if (!s_card) return ESP_OK;
    esp_err_t err = esp_vfs_fat_sdcard_unmount(s_mount, s_card);
    s_card = NULL;
    return err;
}

const sdmmc_card_t *kiban_sd_card(void) { return s_card; }
