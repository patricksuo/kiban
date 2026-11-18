idf_component_register(
    SRCS "probe_main.c" "cmd_board.c" "cmd_epd.c" "cmd_touch.c" "cmd_sensors.c" "cmd_storage.c" "cmd_audio.c" "cmd_power.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES console fatfs sdmmc esp_hw_support app_update esp_psram spi_flash esp_driver_gpio esp_timer heap
)
