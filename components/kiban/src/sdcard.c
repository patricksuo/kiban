// SD card mount. Pins and DAT3 handling from the vendor firmware (MIT, CloudZao/Metalio-E-INK4
// @d12f481 main/boards/metalio-e-ink-4/config.h:29-33) and FreeInk SDK (MIT, MetalioEInk4Board.h:20).
#include "kiban/sdcard.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

static const char *TAG = "kiban.sd";
static sdmmc_card_t *s_card;       // FAT mount (owned by the VFS layer)
static sdmmc_card_t *s_raw;        // raw open (owned here)
static char s_mount[16];

// Host and slot configuration shared by the FAT mount and the raw open.
static esp_err_t sd_config(int max_freq_khz, sdmmc_host_t *host, sdmmc_slot_config_t *slot)
{
    // DAT3 must read high or the card may enter SPI mode; GPIO46 is input-only.
    const gpio_config_t dat3 = {.pin_bit_mask = 1ULL << KIBAN_SD_PIN_DAT3, .mode = GPIO_MODE_INPUT,
                                .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE};
    ESP_RETURN_ON_ERROR(gpio_config(&dat3), TAG, "dat3");

    *host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    host->max_freq_khz = max_freq_khz > 0 ? max_freq_khz : SDMMC_FREQ_DEFAULT;
    *slot = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
    slot->width = 1;
    slot->clk = KIBAN_SD_PIN_CLK;
    slot->cmd = KIBAN_SD_PIN_CMD;
    slot->d0 = KIBAN_SD_PIN_D0;
    slot->flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    return ESP_OK;
}

esp_err_t kiban_sd_mount(const char *mount_point, int max_freq_khz)
{
    if (s_card) return ESP_OK;
    if (s_raw) return ESP_ERR_INVALID_STATE;
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    ESP_RETURN_ON_ERROR(sd_config(max_freq_khz, &host, &slot), TAG, "config");
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

const char *kiban_sd_mount_point(void) { return s_card ? s_mount : NULL; }

// Same as the FAT VFS layer's host deinit (fatfs/vfs/vfs_fat_sdmmc.c call_host_deinit).
static esp_err_t host_deinit(const sdmmc_host_t *host)
{
    return (host->flags & SDMMC_HOST_FLAG_DEINIT_ARG) ? host->deinit_p(host->slot) : host->deinit();
}

esp_err_t kiban_sd_raw_open(int max_freq_khz, sdmmc_card_t **card)
{
    ESP_RETURN_ON_FALSE(card, ESP_ERR_INVALID_ARG, TAG, "card");
    ESP_RETURN_ON_FALSE(!s_card && !s_raw, ESP_ERR_INVALID_STATE, TAG, "card in use");
    sdmmc_host_t host;
    sdmmc_slot_config_t slot;
    ESP_RETURN_ON_ERROR(sd_config(max_freq_khz, &host, &slot), TAG, "config");
    sdmmc_card_t *c = calloc(1, sizeof *c);
    ESP_RETURN_ON_FALSE(c, ESP_ERR_NO_MEM, TAG, "no memory");
    esp_err_t err = host.init();
    if (err != ESP_OK) goto fail_free;
    err = sdmmc_host_init_slot(host.slot, &slot);
    if (err != ESP_OK) goto fail_host;
    err = sdmmc_card_init(&host, c);
    if (err != ESP_OK) goto fail_host;
    s_raw = c;
    *card = c;
    return ESP_OK;
fail_host:
    host_deinit(&host);
fail_free:
    free(c);
    ESP_LOGE(TAG, "raw open failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t kiban_sd_raw_close(void)
{
    if (!s_raw) return ESP_OK;
    esp_err_t err = host_deinit(&s_raw->host);
    free(s_raw);
    s_raw = NULL;
    return err;
}
