idf_component_register(
    SRCS "probe_main.c" "cmd_board.c" "cmd_epd.c" "cmd_touch.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES console app_update esp_psram spi_flash esp_driver_gpio esp_timer heap
)
