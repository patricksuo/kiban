idf_component_register(
    SRCS "probe_main.c" "cmd_board.c" "cmd_epd.c" "cmd_touch.c" "cmd_sensors.c" "cmd_storage.c" "cmd_audio.c" "cmd_power.c" "cmd_wifi.c" "cmd_ble.c" "cmd_store.c" "cmd_usb.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash bootloader_support esp_partition console fatfs sdmmc esp_hw_support esp_wifi esp_netif esp_event bt app_update esp_psram spi_flash esp_driver_gpio esp_timer heap
)
