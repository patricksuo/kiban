// USB drive mode: the SD card is exported to a USB host as a mass-storage device (TinyUSB MSC).
//
// The ESP32-S3 has one USB full-speed PHY on GPIO19/20, shared by USB-Serial/JTAG (console, esptool) and
// USB-OTG (TinyUSB). While the drive is active the PHY belongs to OTG: the USB-Serial/JTAG console and
// esptool auto-reset are gone until kiban_usb_drive_stop() routes the PHY back. Product firmware must give
// the user a way to stop that does not need the console (a key, a timeout).
//
// While active, the card is exported as raw blocks and the device must not touch it: kiban_usb_drive_start()
// unmounts the FAT volume (callers close their files first) and kiban_usb_drive_stop() remounts it at the
// same mount point and clock. The card is never formatted.
//
// Not re-entrant: one owner task calls start/stop. TinyUSB runs its own task while the drive is active
// (the one exception to "kiban creates no hidden tasks").
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Requires kiban_board_init() (USB mux TCA9555 P0.0 high = ESP; checked, never written here) and
// CONFIG_TINYUSB_MSC_ENABLED=y. The SD card may be mounted (it is remounted on stop) or not (it is opened
// raw and stays unmounted after stop). ESP_OK if already active. On failure everything is rolled back:
// PHY on USB-Serial/JTAG, SD mounted as before.
esp_err_t kiban_usb_drive_start(void);

// Detaches from the host, stops TinyUSB, routes the PHY back to USB-Serial/JTAG and remounts the SD card
// if it was mounted at start. When inactive it only routes the PHY to USB-Serial/JTAG (safe to call at
// boot). Blocks ~0.5 s. The host should eject the drive first: stopping while it writes can leave the
// FAT volume inconsistent (to the host it looks like the device was unplugged).
// ESP_ERR_INVALID_STATE if TinyUSB could not be torn down cleanly; start then refuses until a reboot.
esp_err_t kiban_usb_drive_stop(void);

bool kiban_usb_drive_active(void);

// The host has configured the device (enumerated it and selected the configuration).
bool kiban_usb_host_mounted(void);

// The host ejected the drive (Finder eject / `diskutil eject`): its writes are flushed and the medium now
// reports "not present". The product should then call kiban_usb_drive_stop(). A host that only unmounts
// (`diskutil unmount`) or a pulled cable does not set this.
bool kiban_usb_host_ejected(void);

#ifdef __cplusplus
}
#endif
