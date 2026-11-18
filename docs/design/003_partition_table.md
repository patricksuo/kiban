# 003 · Partition table for kiban products (D1)

Status: **draft for review** (2026-11-18). Nothing has been written to the device. Decision D1 (design/002 §6): products
get their own partition table with a product-owned NVS; the stock bootloader stays.

## 1. Constraints

| Constraint | Source |
|---|---|
| Keep the stock bootloader (ESP-IDF 5.5.x). It reads the table at `0x8000` and boots the slot selected by `otadata` | `[source]` vendor sdkconfig: `CONFIG_PARTITION_TABLE_OFFSET=0x8000`; boots `ota_0` today `[measured]` P0 |
| The stock bootloader has app rollback enabled: a newly selected image boots as "pending verify" and must confirm itself | `[source]` vendor sdkconfig `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` (v2.0.63; the device runs v2.0.65's bootloader) |
| Never open the stock `nvs` (vendor Wi-Fi credentials, device identity) | design/002 §2 |
| One firmware on the device at a time (bunko **or** manabu); dual coexistence is deferred | bunko CLAUDE.md |
| Recovery is always possible (ROM download over USB after a real power-off) and the full 16 MB stock backup exists | research/003 B1 |
| Large data (books, dictionaries, audio) lives on the SD card | product scope |

## 2. Stock table (decoded from the device)

| Name | Type / subtype | Offset | Size |
|---|---|---|---|
| nvs | data / nvs | 0x9000 | 16K |
| otadata | data / ota | 0xd000 | 8K |
| phy_init | data / phy | 0xf000 | 4K |
| model | data / spiffs | 0x10000 | 400K |
| ota_0 | app / ota_0 | 0x80000 | 5M |
| ota_1 | app / ota_1 | 0x580000 | 5M |
| resources | data / spiffs | 0xa80000 | 400K |
| font_data | data / 0x40 | 0xae4000 | 5M |
| coredump | data / coredump | 0xfe4000 | 64K |

## 3. Proposed table

Same boundaries as the stock table wherever possible, so that only the 3 KB table at `0x8000` changes and every
region keeps its address. Regions that change role are erased by the product on first use, never the others.

```
# kiban partition table v1 (16 MB). Offsets as in the stock table.
# Name,      Type, SubType,  Offset,   Size
vendor_nvs,  data, 0xfe,     0x9000,   16K    # stock NVS kept byte-for-byte, inert subtype: nothing mounts it
otadata,     data, ota,      0xd000,   8K
phy_init,    data, phy,      0xf000,   4K
nvs,         data, nvs,      0x10000,  448K   # product NVS (was "model" 400K + 48K gap up to 0x80000)
ota_0,       app,  ota_0,    0x80000,  5M
ota_1,       app,  ota_1,    0x580000, 5M
reserved,    data, 0xfd,     0xa80000, 400K   # was "resources"; left untouched, reserved
assets,      data, littlefs, 0xae4000, 5M     # was "font_data": product assets (UI fonts, small data)
coredump,    data, coredump, 0xfe4000, 64K
```

| Region | Role | First-use action |
|---|---|---|
| `vendor_nvs` | Stock NVS, preserved (only the full backup matters for a stock restore, but nothing forces an erase) | none, never opened |
| `nvs` | Product settings, Wi-Fi credentials (product policy), SOC snapshot | product erases this partition only if `nvs_flash_init()` reports no free pages / new version, i.e. on the first boot after the table switch. Never `nvs_flash_erase()` of anything else |
| `ota_0` / `ota_1` | A/B app slots, 5 MB each (probe image 1.3 MB) | none |
| `reserved` | Kept for later (e.g. a board-level record, factory data) | none |
| `assets` | LittleFS for product assets that must work without an SD card (e.g. a Japanese UI font) | formatted by the product on first use (this partition only) |
| `coredump` | Crash dumps (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`) | none |

kiban ships the CSV (`partitions/kiban_16mb_v1.csv`) and a matching `sdkconfig.defaults` fragment; products point
`CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` at it. Both products use the same table, so switching firmware between
bunko and manabu never needs a table change.

## 4. Switching procedure (one-time, per device)

1. Build the probe with the new CSV; verify the generated `partition-table.bin` decodes to §3.
2. Write **only** `0x8000` (partition table) and `ota_0` (probe) with esptool; bootloader and `otadata` untouched.
3. Boot: the stock bootloader reads the new table and boots `ota_0` (same address as today).
4. Probe checks: running partition `ota_0`; `nvs_flash_init()` on the product `nvs` (erase-on-first-use of that
   partition only); write/read a test key; `vendor_nvs` bytes unchanged (compare a read-back against the backup);
   coredump partition found; LittleFS format/mount on `assets`.
5. OTA check: write the probe to `ota_1`, select it, reboot, confirm it marks itself valid (rollback), select `ota_0`
   back.

Rollback to the stock layout: write the stock table bytes from the backup to `0x8000` (or restore the full backup).

## 5. Open questions

1. Does the device's v2.0.65 bootloader really have rollback enabled? (The probe already calls
   `esp_ota_mark_app_valid_cancel_rollback()`, which is harmless either way; step 5 shows it.)
2. NVS size: 448K is generous; it costs a little init time (more pages to scan). Smaller is possible (e.g. 192K) with
   the rest added to `reserved`.
3. `assets` filesystem: LittleFS needs the `joltwallet/littlefs` managed component (not in core IDF); FAT with wear
   levelling is in core IDF. LittleFS is the usual choice for power-loss safety.
