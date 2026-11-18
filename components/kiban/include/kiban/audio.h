// Audio output through the Bluetooth audio module (Metalio E-Ink 4).
// The ESP32-S3 is an I2S *slave*: the module supplies BCLK/WS. The module is told to clock and route
// audio over UART AT commands. The amplifier (TCA9555 P0.4) is off unless kiban_audio_amp(true).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KIBAN_AUDIO_PIN_BCLK   6
#define KIBAN_AUDIO_PIN_WS     43
#define KIBAN_AUDIO_PIN_DOUT   7
#define KIBAN_AUDIO_PIN_DIN    17
#define KIBAN_AUDIO_UART_TX    48
#define KIBAN_AUDIO_UART_RX    47
#define KIBAN_AUDIO_RATE_HZ    16000

// Callback for each text line received from the module (may be NULL).
typedef void (*kiban_audio_line_cb_t)(const char *line, void *ctx);

// UART + I2S slave TX set up; the module is put into local playback mode
// (AT+RX=2, 700 ms, AT+MODE=1). Waits up to wait_ms for "SET MODE 1"; returns ESP_ERR_TIMEOUT if the
// module never confirms (the channel stays usable for diagnosis). Amplifier stays off.
esp_err_t kiban_audio_open(uint32_t wait_ms, kiban_audio_line_cb_t cb, void *ctx);
esp_err_t kiban_audio_close(void);     // amplifier off first, then I2S and UART released

// Send one AT command line ("\r\n" appended) and collect reply lines for wait_ms.
esp_err_t kiban_audio_at(const char *cmd, uint32_t wait_ms);

// Write mono 16-bit PCM at 16 kHz; volume 0..100 (linear). Returns ESP_ERR_TIMEOUT if the module
// provides no I2S clock within timeout_ms (the vendor firmware waits forever here).
esp_err_t kiban_audio_write(const int16_t *pcm, size_t frames, int volume, uint32_t timeout_ms, size_t *written);
uint64_t kiban_audio_frames_written(void);

// Read raw input frames (two 32-bit slots per frame, as delivered by the module). Bounded wait.
esp_err_t kiban_audio_read(int32_t *stereo, size_t frames, uint32_t timeout_ms, size_t *got);

esp_err_t kiban_audio_amp(bool on);    // P0.1 = 0 (ESP path) then P0.4

#ifdef __cplusplus
}
#endif
