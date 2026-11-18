// kiban probe firmware: exercises the Metalio E-Ink 4 hardware from a serial console.
// Safety: no NVS / Wi-Fi init (stock NVS stays untouched), no charger writes,
// no power-off pulses, amplifier stays off.
#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "kiban/board.h"
#include "probe_cmds.h"

static const char *TAG = "probe";

static int cmd_info(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    printf("chip: ESP32-S3 rev v%d.%d, %d cores, features 0x%08lx\n", chip.revision / 100, chip.revision % 100,
           chip.cores, (unsigned long)chip.features);
    printf("flash: %lu bytes\n", (unsigned long)flash);
    printf("psram: %u bytes (%s)\n", (unsigned)esp_psram_get_size(), esp_psram_is_initialized() ? "ok" : "not initialised");
    printf("app: %s %s, idf %s\n", app->project_name, app->version, app->idf_ver);
    printf("running partition: %s @ 0x%lx\n", run ? run->label : "?", run ? (unsigned long)run->address : 0UL);
    printf("reset reason: %d\n", (int)esp_reset_reason());
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_restart();
    return 0;
}

void app_main(void)
{
    // Board bring-up drives the USB mux to the ESP side first thing it can.
    esp_err_t err = kiban_board_init(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board init failed: %s (console still starts)", esp_err_to_name(err));
    }
    // If the stock bootloader has rollback enabled and this image is pending, confirm it.
    esp_ota_mark_app_valid_cancel_rollback();

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "kiban> ";
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &repl_cfg, &repl));

    esp_console_register_help_command();
    const esp_console_cmd_t info = {.command = "info", .help = "chip, flash, PSRAM, app and partition info", .func = cmd_info};
    const esp_console_cmd_t reboot = {.command = "reboot", .help = "software reset", .func = cmd_reboot};
    ESP_ERROR_CHECK(esp_console_cmd_register(&info));
    ESP_ERROR_CHECK(esp_console_cmd_register(&reboot));
    probe_register_board_cmds();
    probe_register_epd_cmds();
    probe_register_touch_cmds();
    probe_register_sensor_cmds();

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
