idf_component_register(
    SRCS "src/board.c" "src/ioexp.c" "src/touch.c" "src/sensors.c" "src/sdcard.c" "src/haptic.c" "src/audio.c" "src/power.c" "src/battery_soc.c" "src/charger.c" "src/battery.c" "src/i2c_dev.c" "src/keys.c" "src/epd.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_driver_gpio esp_driver_sdmmc sdmmc fatfs
    PRIV_REQUIRES log freertos esp_timer esp_driver_spi heap esp_driver_i2s esp_driver_uart
)
