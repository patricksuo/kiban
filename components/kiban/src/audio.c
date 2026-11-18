// BT audio module driver. Sequence and I2S format from the vendor firmware (MIT,
// CloudZao/Metalio-E-INK4 @d12f481): boards/common/audio/bt_audio_codec.cc:20-63,100 (I2S0 slave,
// 32-bit stereo, 16 kHz), display/screen/bluetooth_screen/bluetooth_uart.cc:190-196 (AT+RX=2,
// 700 ms, AT+MODE=1), boards/metalio-e-ink-4/periph/metalio_board_periph.cc:28 (UART2 115200),
// boards/metalio-e-ink-4/config.h:9-20 (pins). Unlike the vendor code every wait is bounded.
#include "kiban/audio.h"

#include <string.h>

#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kiban/ioexp.h"

static const char *TAG = "kiban.audio";
#define UART_PORT UART_NUM_2
#define CHUNK_FRAMES 256

static i2s_chan_handle_t s_tx;
static bool s_uart;
static kiban_audio_line_cb_t s_cb;
static void *s_ctx;
static uint64_t s_frames;
static int32_t *s_buf;

// Read UART for wait_ms, split into lines, report each; returns true if `want` was seen.
static bool pump_lines(uint32_t wait_ms, const char *want)
{
    char line[160];
    size_t n = 0;
    bool seen = false;
    const int64_t end = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    while (esp_timer_get_time() < end) {
        uint8_t c;
        if (uart_read_bytes(UART_PORT, &c, 1, pdMS_TO_TICKS(20)) != 1) continue;
        if (c == '\r' || c == '\n') {
            if (n) {
                line[n] = 0;
                ESP_LOGI(TAG, "module: %s", line);
                if (s_cb) s_cb(line, s_ctx);
                if (want && strstr(line, want)) seen = true;
                n = 0;
            }
        } else if (n + 1 < sizeof line) {
            line[n++] = (char)c;
        }
    }
    if (n) { line[n] = 0; ESP_LOGI(TAG, "module (partial): %s", line); if (s_cb) s_cb(line, s_ctx); }
    return seen;
}

static esp_err_t send_line(const char *cmd)
{
    ESP_LOGI(TAG, "TX: %s", cmd);
    uart_write_bytes(UART_PORT, cmd, strlen(cmd));
    uart_write_bytes(UART_PORT, "\r\n", 2);
    return uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(100));
}

esp_err_t kiban_audio_amp(bool on)
{
    if (on) ESP_RETURN_ON_ERROR(kiban_ioexp_set(KIBAN_IOEXP_PA_SWITCH, false), TAG, "pa switch");
    return kiban_ioexp_set(KIBAN_IOEXP_PA_EN, on);
}

esp_err_t kiban_audio_at(const char *cmd, uint32_t wait_ms)
{
    ESP_RETURN_ON_FALSE(s_uart, ESP_ERR_INVALID_STATE, TAG, "not open");
    uart_flush_input(UART_PORT);
    ESP_RETURN_ON_ERROR(send_line(cmd), TAG, "tx");
    pump_lines(wait_ms, NULL);
    return ESP_OK;
}

esp_err_t kiban_audio_open(uint32_t wait_ms, kiban_audio_line_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_ctx = ctx;
    kiban_audio_amp(false);
    if (!s_uart) {
        const uart_config_t cfg = {.baud_rate = 115200, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                                   .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                                   .source_clk = UART_SCLK_DEFAULT};
        ESP_RETURN_ON_ERROR(uart_driver_install(UART_PORT, 1024, 0, 0, NULL, 0), TAG, "uart install");
        ESP_RETURN_ON_ERROR(uart_param_config(UART_PORT, &cfg), TAG, "uart cfg");
        ESP_RETURN_ON_ERROR(uart_set_pin(UART_PORT, KIBAN_AUDIO_UART_TX, KIBAN_AUDIO_UART_RX, UART_PIN_NO_CHANGE,
                                         UART_PIN_NO_CHANGE), TAG, "uart pins");
        s_uart = true;
    }
    if (!s_tx) {
        i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
        chan.auto_clear_after_cb = true;
        ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &s_tx, NULL), TAG, "i2s channel");
        i2s_std_config_t std = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(KIBAN_AUDIO_RATE_HZ),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = KIBAN_AUDIO_PIN_BCLK, .ws = KIBAN_AUDIO_PIN_WS,
                         .dout = KIBAN_AUDIO_PIN_DOUT, .din = I2S_GPIO_UNUSED},
        };
        // Match the vendor slot layout: 32-bit WS width, left aligned, MSB first.
        std.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_32BIT;
        std.slot_cfg.bit_shift = true;
        std.slot_cfg.left_align = true;
        ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std), TAG, "i2s std");
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s enable");
    }
    if (!s_buf) {
        s_buf = heap_caps_malloc(CHUNK_FRAMES * 2 * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        ESP_RETURN_ON_FALSE(s_buf, ESP_ERR_NO_MEM, TAG, "buffer");
    }
    uart_flush_input(UART_PORT);
    send_line("AT+RX=2");
    pump_lines(700, NULL);
    send_line("AT+MODE=1");
    const bool ok = pump_lines(wait_ms, "SET MODE 1");
    if (!ok) ESP_LOGW(TAG, "module did not confirm SET MODE 1 within %lu ms", (unsigned long)wait_ms);
    return ok ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t kiban_audio_write(const int16_t *pcm, size_t frames, int volume, uint32_t timeout_ms, size_t *written)
{
    ESP_RETURN_ON_FALSE(s_tx && s_buf, ESP_ERR_INVALID_STATE, TAG, "not open");
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    const int64_t gain = (int64_t)volume * 65536 / 100;   // 16-bit sample -> 32-bit slot
    size_t done = 0;
    const int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (done < frames) {
        const size_t n = frames - done > CHUNK_FRAMES ? CHUNK_FRAMES : frames - done;
        for (size_t i = 0; i < n; i++) {
            const int32_t v = (int32_t)((int64_t)pcm[done + i] * gain);
            s_buf[2 * i] = v;
            s_buf[2 * i + 1] = v;
        }
        const int64_t left_us = end - esp_timer_get_time();
        if (left_us <= 0) break;
        size_t bytes = 0;
        esp_err_t err = i2s_channel_write(s_tx, s_buf, n * 2 * sizeof(int32_t), &bytes, (uint32_t)(left_us / 1000) + 1);
        done += bytes / (2 * sizeof(int32_t));
        s_frames += bytes / (2 * sizeof(int32_t));
        if (err == ESP_ERR_TIMEOUT) break;
        ESP_RETURN_ON_ERROR(err, TAG, "i2s write");
    }
    if (written) *written = done;
    return done == frames ? ESP_OK : ESP_ERR_TIMEOUT;
}

uint64_t kiban_audio_frames_written(void) { return s_frames; }

esp_err_t kiban_audio_close(void)
{
    kiban_audio_amp(false);
    if (s_tx) {
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    if (s_uart) {
        uart_driver_delete(UART_PORT);
        s_uart = false;
    }
    free(s_buf);
    s_buf = NULL;
    return ESP_OK;
}
