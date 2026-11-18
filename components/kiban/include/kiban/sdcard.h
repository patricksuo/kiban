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

#ifdef __cplusplus
}
#endif
