# bench_core.c is portable (no ESP-IDF headers; ../host/host_bench.c builds it on a host); bench_main.c is the device side.
idf_component_register(
    SRCS "bench_main.c" "bench_core.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES console fatfs sdmmc esp_timer app_update heap esp_hw_support esp_rom esp_psram spi_flash
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wextra -Wshadow)
