// SD card over SDMMC 1-bit (CLK38 CMD40 D0=39, DAT3=GPIO46 held high) and FAT VFS.
// Never formats: a card that does not mount is reported, not repaired.
#pragma once

#include "esp_err.h"
#include "sdmmc_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KIBAN_SD_PIN_CLK  38
#define KIBAN_SD_PIN_CMD  40
#define KIBAN_SD_PIN_D0   39
#define KIBAN_SD_PIN_DAT3 46

// Requires the panel/SD rail on (kiban_board_init()). mount_point e.g. "/sd".
esp_err_t kiban_sd_mount(const char *mount_point, int max_freq_khz);
esp_err_t kiban_sd_unmount(void);
const sdmmc_card_t *kiban_sd_card(void);   // NULL when not mounted
// Mount point given to the current kiban_sd_mount(), NULL when not mounted.
const char *kiban_sd_mount_point(void);

// Raw block access without a filesystem (used by kiban_usb to export the card over USB).
// Initialises the SDMMC host and the card with the same pins and DAT3 handling as kiban_sd_mount().
// ESP_ERR_INVALID_STATE while the FAT volume is mounted or a raw handle is already open; kiban_sd_mount()
// returns ESP_ERR_INVALID_STATE while a raw handle is open. The card stays owned by kiban: release it with
// kiban_sd_raw_close(), never free it.
esp_err_t kiban_sd_raw_open(int max_freq_khz, sdmmc_card_t **card);
esp_err_t kiban_sd_raw_close(void);

#ifdef __cplusplus
}
#endif
