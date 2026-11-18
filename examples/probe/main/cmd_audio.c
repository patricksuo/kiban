// P6 commands: BT audio module. The amplifier is on only inside `audio tone`, and off on every exit.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "kiban/audio.h"
#include "probe_cmds.h"

static bool s_open;

static void on_line(const char *line, void *ctx) { (void)ctx; printf("  < %s\n", line); }

static int ensure_open(void)
{
    if (s_open) return 0;
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = kiban_audio_open(2000, on_line, NULL);
    printf("open: %s (%lld ms)\n", esp_err_to_name(err), (long long)((esp_timer_get_time() - t0) / 1000));
    s_open = true;   // channel is usable even without confirmation, for diagnosis
    return 0;
}

// Write `ms` of signal; returns elapsed ms and frames written.
static esp_err_t play(int hz, int ms, int vol, int64_t *elapsed_ms, size_t *frames_out)
{
    const size_t frames = (size_t)KIBAN_AUDIO_RATE_HZ * (size_t)ms / 1000;
    int16_t *pcm = heap_caps_malloc(frames * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < frames; i++)
        pcm[i] = hz > 0 ? (int16_t)(32767.0f * sinf(2.0f * 3.14159265f * (float)hz * (float)i / KIBAN_AUDIO_RATE_HZ)) : 0;
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = kiban_audio_write(pcm, frames, vol, (uint32_t)ms + 1000, frames_out);
    *elapsed_ms = (esp_timer_get_time() - t0) / 1000;
    free(pcm);
    return err;
}

static int cmd_audio(int argc, char **argv)
{
    if (argc < 2) { printf("usage: audio open | at <CMD> | clk | tone [hz] [ms] [vol%%] | close\n"); return 1; }
    if (!strcmp(argv[1], "open")) { s_open = false; return ensure_open(); }
    if (!strcmp(argv[1], "close")) { kiban_audio_close(); s_open = false; printf("closed, amp off\n"); return 0; }
    ensure_open();
    if (!strcmp(argv[1], "at") && argc > 2) {
        char cmd[96] = "";
        for (int i = 2; i < argc; i++) { strlcat(cmd, argv[i], sizeof cmd); if (i + 1 < argc) strlcat(cmd, " ", sizeof cmd); }
        kiban_audio_at(cmd, 1500);
        return 0;
    }
    if (!strcmp(argv[1], "clk")) {
        // Silent, amplifier off: if the module clocks I2S at 16 kHz, 1 s of samples takes ~1 s (after DMA fill).
        int64_t el; size_t fr = 0;
        esp_err_t err = play(0, 1000, 0, &el, &fr);
        printf("clk: %s, wrote %u/16000 frames in %lld ms\n", esp_err_to_name(err), (unsigned)fr, (long long)el);
        return 0;
    }
    if (!strcmp(argv[1], "tone")) {
        const int hz = argc > 2 ? atoi(argv[2]) : 440, ms = argc > 3 ? atoi(argv[3]) : 1000;
        int vol = argc > 4 ? atoi(argv[4]) : 5;
        if (vol > 30) vol = 30;   // probe cap
        printf("tone %d Hz, %d ms, volume %d%%\n", hz, ms, vol);
        kiban_audio_amp(true);
        int64_t el; size_t fr = 0;
        esp_err_t err = play(hz, ms, vol, &el, &fr);
        // let the DMA drain before cutting the amp
        int64_t el2; size_t fr2 = 0;
        play(0, 200, 0, &el2, &fr2);
        kiban_audio_amp(false);
        printf("tone: %s, wrote %u frames in %lld ms; amp off\n", esp_err_to_name(err), (unsigned)fr, (long long)el);
        return 0;
    }
    printf("unknown subcommand\n");
    return 1;
}

void probe_register_audio_cmds(void)
{
    const esp_console_cmd_t cmd = {.command = "audio", .help = "audio open | at <CMD> | clk | tone [hz] [ms] [vol%] | close", .func = cmd_audio};
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
