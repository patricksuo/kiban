idf_component_register(
    SRCS "src/board.c" "src/ioexp.c" "src/touch.c" "src/sensors.c" "src/epd.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_driver_gpio
    PRIV_REQUIRES log freertos esp_timer esp_driver_spi heap
)
