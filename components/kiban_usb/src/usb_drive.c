// USB drive mode (SD card as USB mass storage). Ported to C from the vendor firmware (MIT, CloudZao/Metalio-E-INK4
// @d12f481 main/boards/common/storage/usb_virtual_disk.cc): PHY routing (RoutePhyToOtg / RoutePhyToUsj
// :194-220), manual OTG PHY + TinyUSB install (:517-583), teardown order and delays (:444-515, :588-610),
// descriptors (:96-143). Differences: the card is exported only to the USB host (no APP/USB mount
// switching, so esp_tinyusb never mounts or formats it), no worker task or UI state, the SD card is
// handed over through kiban_sd_raw_open() and remounted through kiban_sd_mount().
#include "kiban_usb/usb_drive.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "kiban/ioexp.h"
#include "kiban/sdcard.h"
#include "sdkconfig.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "kiban_usb supports the ESP32-S3 only"
#endif
#if !CONFIG_TINYUSB_MSC_ENABLED
#error "kiban_usb needs CONFIG_TINYUSB_MSC_ENABLED=y in the product's sdkconfig.defaults"
#endif

static const char *TAG = "kiban.usb";

// Drain time for MSC writes that esp_tinyusb defers to its task, between detaching and stopping the task.
#define DRAIN_MS        200
// After the OTG PHY is gone and before USB-Serial/JTAG takes it over; vendor :460-462.
#define PHY_OFF_MS      150
#define PHY_SETTLE_MS   100

static bool s_active;
static bool s_broken;                 // teardown failed: refuse to start until reboot
static volatile bool s_host_mounted;  // written by the TinyUSB task
static volatile bool s_host_ejected;  // host sent SCSI START STOP UNIT with eject (written by the TinyUSB task)
static usb_phy_handle_t s_phy;
static tinyusb_msc_storage_handle_t s_storage;
static char s_remount[16];            // mount point to restore on stop ("" = was not mounted)
static int s_freq_khz;

enum { ITF_MSC = 0, ITF_TOTAL };
enum { EP_MSC_OUT = 0x01, EP_MSC_IN = 0x81 };
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = TINYUSB_ESPRESSIF_VID,
    .idProduct = 0x4002,                // as in the vendor firmware (:119)
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),
    TUD_MSC_DESCRIPTOR(ITF_MSC, 0, EP_MSC_OUT, EP_MSC_IN, 64),
};

static const char *s_strings[] = {
    (const char[]){0x09, 0x04},  // English
    "kiban",
    "E-Ink 4 SD card",
    "000001",
    "MSC",
};

// usb_new_phy(OTG) sets RTC_CNTL sw_usb_phy_sel; enabling the USJ pad alone is not enough (vendor :211).
static void route_phy_to_usj(void)
{
    usb_serial_jtag_ll_phy_enable_external(false);
    usb_serial_jtag_ll_phy_enable_pad(true);
}

static void route_phy_to_otg(void)
{
    usb_serial_jtag_ll_phy_enable_pad(false);
    usb_serial_jtag_ll_phy_enable_external(true);
}

static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event->id == TINYUSB_EVENT_ATTACHED) s_host_mounted = true;
    else if (event->id == TINYUSB_EVENT_DETACHED) s_host_mounted = false;
}

// On a host eject (SCSI START STOP UNIT, LoEj=1, Start=0) esp_tinyusb takes the medium back for the app
// (mount_point APP), so TEST UNIT READY reports "medium not present" and the host does not remount it.
// Without that (auto_mount_off) the unit stays ready and macOS remounts the volume right after an eject,
// which looks like Finder's eject doing nothing [measured]. The app-side FAT mount is private to this
// component, never formats (do_not_format), and is dropped when the storage is deleted on stop.
// MOUNT_COMPLETE / FORMAT_REQUIRED / MOUNT_FAILED after start all mean: the host let go of the medium.
static void msc_event_cb(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event, void *arg)
{
    (void)handle;
    (void)arg;
    if (event->id == TINYUSB_MSC_EVENT_MOUNT_COMPLETE || event->id == TINYUSB_MSC_EVENT_FORMAT_REQUIRED ||
        event->id == TINYUSB_MSC_EVENT_MOUNT_FAILED)
        s_host_ejected = true;
    if (event->id != TINYUSB_MSC_EVENT_MOUNT_COMPLETE) ESP_LOGW(TAG, "msc event %d", (int)event->id);
}

// Restore the SD card to its state before start. Returns the remount result. Not after a failed MSC
// release: the leaked storage object still points at the raw card, so the card stays open until reboot.
static esp_err_t sd_restore(void)
{
    if (s_broken) return ESP_ERR_INVALID_STATE;
    kiban_sd_raw_close();
    if (!s_remount[0]) return ESP_OK;
    esp_err_t err = kiban_sd_mount(s_remount, s_freq_khz);
    if (err != ESP_OK) ESP_LOGE(TAG, "remount %s failed: %s", s_remount, esp_err_to_name(err));
    return err;
}

static void msc_release(void)
{
    if (s_storage) {
        if (tinyusb_msc_delete_storage(s_storage) != ESP_OK) {
            // Pending deferred writes or a stuck LUN: the storage object cannot be freed.
            ESP_LOGE(TAG, "msc storage not released");
            s_broken = true;
            return;
        }
        s_storage = NULL;
    }
    tinyusb_msc_uninstall_driver();
}

esp_err_t kiban_usb_drive_start(void)
{
    if (s_active) return ESP_OK;
    ESP_RETURN_ON_FALSE(!s_broken, ESP_ERR_INVALID_STATE, TAG, "previous stop failed; reboot first");
    bool mux_esp = false;
    ESP_RETURN_ON_ERROR(kiban_ioexp_get_input(KIBAN_IOEXP_USB_MUX_SEL, &mux_esp), TAG, "usb mux read");
    ESP_RETURN_ON_FALSE(mux_esp, ESP_ERR_INVALID_STATE, TAG, "usb mux not on ESP (P0.0 low)");

    // Remember how the card was mounted, then hand it over as raw blocks.
    const sdmmc_card_t *mounted = kiban_sd_card();
    const char *mp = kiban_sd_mount_point();
    s_remount[0] = '\0';
    s_freq_khz = 0;
    if (mounted && mp) {
        strncpy(s_remount, mp, sizeof s_remount - 1);
        s_remount[sizeof s_remount - 1] = '\0';
        s_freq_khz = mounted->host.max_freq_khz;   // copy of the host config passed at mount
    }
    esp_err_t err = kiban_sd_unmount();
    if (err != ESP_OK) ESP_LOGW(TAG, "unmount: %s", esp_err_to_name(err));
    sdmmc_card_t *card = NULL;
    err = kiban_sd_raw_open(s_freq_khz, &card);
    if (err != ESP_OK) {
        sd_restore();
        return err;
    }

    const tinyusb_msc_driver_config_t drv = {
        .user_flags.auto_mount_off = 0,   // needed for host eject (see msc_event_cb); never formats
        .callback = msc_event_cb,
    };
    err = tinyusb_msc_install_driver(&drv);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc driver: %s", esp_err_to_name(err));
        sd_restore();
        return err;
    }
    tinyusb_msc_storage_config_t st = {
        .medium.card = card,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        .fat_fs = {.base_path = "/kiban_usb", .do_not_format = true, .config = {.format_if_mount_failed = false, .max_files = 1}},
    };
    err = tinyusb_msc_new_storage_sdmmc(&st, &s_storage);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc storage: %s", esp_err_to_name(err));
        s_storage = NULL;
        tinyusb_msc_uninstall_driver();
        sd_restore();
        return err;
    }

    // From here the console is gone until the PHY is routed back.
    route_phy_to_otg();
    const usb_phy_config_t phy_cfg = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_FULL,
    };
    err = usb_new_phy(&phy_cfg, &s_phy);   // also sets GPIO19/20 drive strength 3 (usb_phy.c)
    if (err != ESP_OK) {
        s_phy = NULL;
        route_phy_to_usj();
        ESP_LOGE(TAG, "usb phy: %s", esp_err_to_name(err));
        msc_release();
        sd_restore();
        return err;
    }

    tinyusb_config_t cfg = TINYUSB_CONFIG_FULL_SPEED(usb_event_cb, NULL);
    cfg.phy.skip_setup = true;             // PHY owned here so it can be handed back to USB-Serial/JTAG
    cfg.descriptor.device = &s_device_desc;
    cfg.descriptor.full_speed_config = s_config_desc;
    cfg.descriptor.string = s_strings;
    cfg.descriptor.string_count = sizeof s_strings / sizeof s_strings[0];
    s_host_mounted = false;
    s_host_ejected = false;
    err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) {
        usb_del_phy(s_phy);
        s_phy = NULL;
        vTaskDelay(pdMS_TO_TICKS(PHY_OFF_MS));
        route_phy_to_usj();
        vTaskDelay(pdMS_TO_TICKS(PHY_SETTLE_MS));
        ESP_LOGE(TAG, "tinyusb: %s", esp_err_to_name(err));
        msc_release();
        sd_restore();
        return err;
    }
    s_active = true;
    return ESP_OK;
}

esp_err_t kiban_usb_drive_stop(void)
{
    if (!s_active) {
        route_phy_to_usj();
        return ESP_OK;
    }
    // Detach first so the host stops issuing commands, then let deferred writes reach the card.
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(DRAIN_MS));
    esp_err_t first = tinyusb_driver_uninstall();
    if (first != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb uninstall: %s", esp_err_to_name(first));
        s_broken = true;
    }
    if (s_phy) {
        usb_del_phy(s_phy);
        s_phy = NULL;
    }
    vTaskDelay(pdMS_TO_TICKS(PHY_OFF_MS));
    route_phy_to_usj();
    vTaskDelay(pdMS_TO_TICKS(PHY_SETTLE_MS));
    s_active = false;
    s_host_mounted = false;
    s_host_ejected = false;

    if (s_broken) {
        // The TinyUSB task may still hold the card: leave it exported-but-detached rather than remount.
        ESP_LOGE(TAG, "usb teardown incomplete; SD left unmounted until reboot");
        return ESP_ERR_INVALID_STATE;
    }
    msc_release();
    if (s_broken) ESP_LOGE(TAG, "usb teardown incomplete; SD left unmounted until reboot");
    return sd_restore();
}

bool kiban_usb_drive_active(void) { return s_active; }

bool kiban_usb_host_mounted(void) { return s_active && s_host_mounted; }

bool kiban_usb_host_ejected(void) { return s_active && s_host_ejected; }
