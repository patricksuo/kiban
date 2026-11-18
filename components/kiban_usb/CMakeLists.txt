idf_component_register(
    SRCS "src/usb_drive.c"
    INCLUDE_DIRS "include"
    REQUIRES kiban
    PRIV_REQUIRES log freertos esp_hw_support esp_hal_usb sdmmc espressif__esp_tinyusb
)
