// P6 commands: BT audio module. The amplifier is on only inside `audio tone`, and off on every exit.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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


// ---- latency: play a short beep and find it in the module's input stream --------------------
#define BIN_FRAMES 16                  // 1 ms at 16 kHz
#define CAP_MS 1200
#define CHUNK 48                       // 3 ms
typedef struct {
    uint32_t env[2][CAP_MS];           // per-ms peak |sample| >> 12, per slot
    int64_t t_end[CAP_MS];             // estimated capture time of the end of each bin (us)
    int bins;
    volatile bool done;
} cap_t;

static void cap_task(void *arg)
{
    cap_t *c = arg;
    int32_t *buf = heap_caps_malloc(CHUNK * 2 * sizeof(int32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    size_t got;
    // Drain whatever is queued so subsequent reads block on fresh samples.
    for (int i = 0; i < 30; i++) kiban_audio_read(buf, CHUNK, 50, &got);
    c->bins = 0;
    while (c->bins + CHUNK / BIN_FRAMES <= CAP_MS) {
        if (kiban_audio_read(buf, CHUNK, 100, &got) != ESP_OK || got != CHUNK) break;
        const int64_t now = esp_timer_get_time();
        for (int b = 0; b < CHUNK / BIN_FRAMES; b++) {
            uint32_t m[2] = {0, 0};
            for (int f = b * BIN_FRAMES; f < (b + 1) * BIN_FRAMES; f++)
                for (int s = 0; s < 2; s++) {
                    int32_t v = buf[2 * f + s] >> 12;
                    uint32_t a = (uint32_t)(v < 0 ? -v : v);
                    if (a > m[s]) m[s] = a;
                }
            c->env[0][c->bins] = m[0];
            c->env[1][c->bins] = m[1];
            c->t_end[c->bins] = now - (int64_t)(CHUNK - (b + 1) * BIN_FRAMES) * 1000000 / KIBAN_AUDIO_RATE_HZ;
            c->bins++;
        }
    }
    free(buf);
    c->done = true;
    vTaskDelete(NULL);
}

static void latency_once(int run, int vol)
{
    cap_t *c = heap_caps_calloc(1, sizeof(cap_t), MALLOC_CAP_SPIRAM);
    const size_t frames = KIBAN_AUDIO_RATE_HZ * 80 / 1000;
    int16_t *pcm = heap_caps_malloc(frames * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!c || !pcm) { printf("no memory\n"); free(c); free(pcm); return; }
    for (size_t i = 0; i < frames; i++) pcm[i] = (int16_t)(32767.0f * sinf(2.0f * 3.14159265f * 1000.0f * (float)i / KIBAN_AUDIO_RATE_HZ));
    xTaskCreatePinnedToCore(cap_task, "cap", 4096, c, 10, NULL, 1);
    vTaskDelay(pdMS_TO_TICKS(500));            // drain + ~300 ms of noise floor
    kiban_audio_amp(true);
    vTaskDelay(pdMS_TO_TICKS(250));            // amplifier settle (its pop lands here)
    const int64_t t_call = esp_timer_get_time();
    size_t w = 0;
    kiban_audio_write(pcm, frames, vol, 1000, &w);
    const int64_t t_ret = esp_timer_get_time();
    while (!c->done) vTaskDelay(pdMS_TO_TICKS(20));
    kiban_audio_amp(false);
    // Noise floor: bins that ended before the amp came on; onset: first bin after t_call above threshold.
    for (int s = 0; s < 2; s++) {
        uint32_t floor = 1;
        int onset = -1;
        uint32_t peak = 0;
        for (int b = 0; b < c->bins; b++) if (c->t_end[b] < t_call - 260000 && c->env[s][b] > floor) floor = c->env[s][b];
        const uint32_t thr = floor * 4 + 50;
        for (int b = 0; b < c->bins; b++) {
            if (c->t_end[b] < t_call) continue;
            if (c->env[s][b] > peak) peak = c->env[s][b];
            if (onset < 0 && c->env[s][b] > thr) onset = b;
        }
        if (onset >= 0)
            printf("run %d slot %d: onset %+.1f ms after write call (floor %lu, thr %lu, peak %lu)\n", run, s,
                   (double)(c->t_end[onset] - t_call) / 1000.0, (unsigned long)floor, (unsigned long)thr, (unsigned long)peak);
        else
            printf("run %d slot %d: no onset (floor %lu, peak %lu)\n", run, s, (unsigned long)floor, (unsigned long)peak);
    }
    printf("run %d: write returned after %.1f ms, %u frames; bins captured %d\n", run, (double)(t_ret - t_call) / 1000.0,
           (unsigned)w, c->bins);
    free(pcm);
    free(c);
}

static int cmd_audio(int argc, char **argv)
{
    if (argc < 2) { printf("usage: audio open | at <CMD> | clk | tone [hz] [ms] [vol%%] | latency [n] [vol%%] | close\n"); return 1; }
    if (!strcmp(argv[1], "open")) {
        if (argc > 3) { kiban_audio_set_dma((uint32_t)atoi(argv[2]), (uint32_t)atoi(argv[3])); printf("dma %s x %s frames\n", argv[2], argv[3]); }
        kiban_audio_close();
        s_open = false;
        return ensure_open();
    }
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
    if (!strcmp(argv[1], "latency")) {
        const int n = argc > 2 ? atoi(argv[2]) : 5;
        const int vol = argc > 3 ? atoi(argv[3]) : 5;
        for (int i = 1; i <= n && i <= 20; i++) latency_once(i, vol > 30 ? 30 : vol);
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
    const esp_console_cmd_t cmd = {.command = "audio", .help = "audio open | at <CMD> | clk | tone [hz] [ms] [vol%] | latency [n] [vol%] | close", .func = cmd_audio};
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
