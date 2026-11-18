// Internal: shared, locked I2C device handles for the board's register-mapped chips.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define KIBAN_I2C_XFER_TIMEOUT_MS 50

esp_err_t kiban_i2c_rd(uint8_t addr, uint8_t reg, uint8_t *buf, size_t n);
esp_err_t kiban_i2c_wr(uint8_t addr, uint8_t reg, uint8_t val);
esp_err_t kiban_i2c_wr16(uint8_t addr, uint8_t reg, uint16_t val);   // little-endian word, one transaction
