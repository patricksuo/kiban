// usb drive start [seconds] | stop | state: SD card as a USB drive (kiban_usb).
// While the drive is active the USB PHY belongs to USB-OTG, so this console (USB-Serial/JTAG) is gone and
// "usb drive stop" cannot be typed. "start" therefore blocks this command until the timeout expires or the
// POWER key is pressed, then stops the drive itself and the console re-enumerates.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kiban/keys.h"
#include "kiban_usb/usb_drive.h"
#include "probe_cmds.h"

#define DEFAULT_SECS 120
#define MAX_SECS     3600

// Last session, printed by "usb drive state" once the console is back.
static struct {
    bool valid;
    esp_err_t start_err, stop_err;
    int64_t active_ms;
    bool host_seen;
    const char *ended_by;
} s_last;

static void print_state(void)
{
    printf("drive: %s, host configured: %s\n", kiban_usb_drive_active() ? "active" : "inactive",
           kiban_usb_host_mounted() ? "yes" : "no");
    if (!s_last.valid) return;
    printf("last session: start %s, active %lld ms, host configured %s, ended by %s, stop %s\n",
           esp_err_to_name(s_last.start_err), (long long)s_last.active_ms, s_last.host_seen ? "yes" : "no",
           s_last.ended_by, esp_err_to_name(s_last.stop_err));
}

static int drive_start(int secs)
{
    const kiban_keys_cfg_t kcfg = {.debounce_ms = 20, .long_ms = 600, .cover_keys = false};
    esp_err_t err = kiban_keys_init(&kcfg);
    if (err != ESP_OK) { printf("keys init: %s\n", esp_err_to_name(err)); return 1; }

    printf("USB drive mode for %d s. The console disappears now (the USB PHY switches to OTG).\n"
           "Eject the drive on the host before it ends. It ends on host eject, after %d s or on a POWER key press,\n"
           "then the console comes back (reconnect the serial monitor).\n", secs, secs);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));   // let the text reach the host before the PHY switches

    memset(&s_last, 0, sizeof s_last);
    s_last.valid = true;
    s_last.ended_by = "-";
    s_last.start_err = kiban_usb_drive_start();
    if (s_last.start_err != ESP_OK) {
        printf("start failed: %s (console unchanged)\n", esp_err_to_name(s_last.start_err));
        return 1;
    }
    const int64_t t0 = esp_timer_get_time(), end = t0 + (int64_t)secs * 1000000;
    s_last.ended_by = "timeout";
    while (esp_timer_get_time() < end) {
        kiban_key_event_t ev[4];
        const size_t n = kiban_keys_poll(esp_timer_get_time(), ev, 4);
        bool power = false;
        for (size_t i = 0; i < n; i++)
            if (ev[i].key == KIBAN_KEY_POWER && ev[i].action == KIBAN_KEY_PRESS) power = true;
        if (power) { s_last.ended_by = "POWER key"; break; }
        if (kiban_usb_host_mounted()) s_last.host_seen = true;
        if (kiban_usb_host_ejected()) { s_last.ended_by = "host eject"; break; }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    s_last.active_ms = (esp_timer_get_time() - t0) / 1000;
    s_last.stop_err = kiban_usb_drive_stop();
    vTaskDelay(pdMS_TO_TICKS(1000));  // give the host time to re-enumerate the console
    printf("USB drive mode ended\n");
    print_state();
    return s_last.stop_err == ESP_OK ? 0 : 1;
}

static int cmd_usb(int argc, char **argv)
{
    if (argc < 3 || strcmp(argv[1], "drive") != 0) {
        printf("usage: usb drive start [seconds] | stop | state\n");
        return 1;
    }
    if (!strcmp(argv[2], "start")) {
        int secs = argc > 3 ? atoi(argv[3]) : DEFAULT_SECS;
        if (secs <= 0 || secs > MAX_SECS) secs = DEFAULT_SECS;
        return drive_start(secs);
    }
    if (!strcmp(argv[2], "stop")) {
        const esp_err_t err = kiban_usb_drive_stop();
        printf("stop: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcmp(argv[2], "state")) { print_state(); return 0; }
    printf("unknown subcommand\n");
    return 1;
}

void probe_register_usb_cmds(void)
{
    const esp_console_cmd_t cmd = {
        .command = "usb",
        .help = "usb drive start [seconds] | stop | state: export the SD card as a USB drive. The console is gone "
                "while active; it ends after [seconds] (default 120, max 3600) or on a POWER key press",
        .func = cmd_usb,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
