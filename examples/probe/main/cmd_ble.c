// BLE RF check with NimBLE: passive scan, report counts and RSSI only (no addresses or names: the repo
// is public). NimBLE persistence is off (CONFIG_BT_NIMBLE_NVS_PERSIST=n): the stock NVS is not touched.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "probe_cmds.h"

#define MAX_SEEN 128
static uint8_t s_seen[MAX_SEEN][6];
static int s_unique, s_reports, s_best, s_worst, s_hist[5];   // >-60, -60..-70, -70..-80, -80..-90, <-90
static SemaphoreHandle_t s_sync, s_done;
static bool s_up;

static void on_sync(void) { xSemaphoreGive(s_sync); }

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    if (ev->type == BLE_GAP_EVENT_DISC) {
        s_reports++;
        const int r = ev->disc.rssi;
        if (r > s_best) s_best = r;
        if (r < s_worst) s_worst = r;
        bool known = false;
        for (int i = 0; i < s_unique && !known; i++) known = !memcmp(s_seen[i], ev->disc.addr.val, 6);
        if (!known && s_unique < MAX_SEEN) {
            memcpy(s_seen[s_unique++], ev->disc.addr.val, 6);
            s_hist[r > -60 ? 0 : r > -70 ? 1 : r > -80 ? 2 : r > -90 ? 3 : 4]++;
        }
    } else if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        xSemaphoreGive(s_done);
    }
    return 0;
}

static esp_err_t ble_up(void)
{
    if (s_up) return ESP_OK;
    if (!s_sync) { s_sync = xSemaphoreCreateBinary(); s_done = xSemaphoreCreateBinary(); }
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    if (xSemaphoreTake(s_sync, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    s_up = true;
    return ESP_OK;
}

static int cmd_ble(int argc, char **argv)
{
    const int secs = argc > 1 ? atoi(argv[1]) : 10;
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = ble_up();
    printf("ble init: %s (%lld ms)\n", esp_err_to_name(err), (long long)((esp_timer_get_time() - t0) / 1000));
    if (err != ESP_OK) return 1;
    uint8_t own;
    if (ble_hs_id_infer_auto(0, &own) != 0) own = BLE_OWN_ADDR_PUBLIC;
    s_unique = s_reports = 0;
    s_best = -127;
    s_worst = 0;
    memset(s_hist, 0, sizeof s_hist);
    const struct ble_gap_disc_params p = {.passive = 1, .filter_duplicates = 0};
    int rc = ble_gap_disc(own, secs * 1000, &p, gap_event, NULL);
    if (rc != 0) { printf("scan start rc=%d\n", rc); return 1; }
    xSemaphoreTake(s_done, pdMS_TO_TICKS((secs + 2) * 1000));
    printf("ble scan %d s: %d devices, %d reports; RSSI best %d dBm, worst %d dBm\n", secs, s_unique, s_reports,
           s_unique ? s_best : 0, s_unique ? s_worst : 0);
    printf("devices by RSSI: >-60:%d  -60..-70:%d  -70..-80:%d  -80..-90:%d  <-90:%d\n", s_hist[0], s_hist[1],
           s_hist[2], s_hist[3], s_hist[4]);
    memset(s_seen, 0, sizeof s_seen);
    return 0;
}

void probe_register_ble_cmds(void)
{
    const esp_console_cmd_t cmd = {.command = "ble", .help = "ble [seconds]: passive BLE scan (counts / RSSI only)", .func = cmd_ble};
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
