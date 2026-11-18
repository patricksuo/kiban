# 003 · Partition table for kiban products (D1)

Status: **applied to the device** (2026-11-18; checks in research/003 "Partition table v1"). Decision D1 (design/002 §6): products
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

## 3. Table (plan B, chosen by the user 2026-11-18)

App code cannot move to the SD card, data can: space goes to the app slots first. CrossPoint-family Metalio images are
5.4–6.1 MB (fonts compiled in); kiban's probe with Wi-Fi, BLE, TLS and all drivers is 1.3 MB.

```
# kiban partition table v1 (16 MB), plan B: 6 MB app slots.
# Name,      Type, SubType,  Offset,   Size
vendor_nvs,  data, 0xfe,     0x9000,   16K     # stock NVS kept byte-for-byte, inert subtype: nothing mounts it
otadata,     data, ota,      0xd000,   8K
phy_init,    data, phy,      0xf000,   4K
nvs,         data, nvs,      0x10000,  192K    # product NVS
ota_0,       app,  ota_0,    0x40000,  6M
ota_1,       app,  ota_1,    0x640000, 6M
assets,      data, littlefs, 0xc40000, 3776K   # product assets (UI font subsets)
coredump,    data, coredump, 0xff0000, 64K
```

| Region | Role | First-use action |
|---|---|---|
| `vendor_nvs` | Stock NVS, preserved | none, never opened |
| `nvs` | Product settings, Wi-Fi credentials (product policy), SOC snapshot | the product erases this partition only, on the first `nvs_flash_init()` that reports no free pages / new version (it held stock data) |
| `ota_0` / `ota_1` | A/B app slots, 6 MB each | none |
| `assets` | LittleFS: assets that must work without an SD card. Planned: Japanese (Jōyō + kana, ~2300 glyphs) and Chinese (3500 common, ~3600 glyphs) UI subsets, kept separate because the glyph forms differ. Estimate `[inference]`: ~3 MB as two TTF subsets rendered on device, ~1–2 MB as bitmaps at two sizes / 2 bpp. Full sets, rare characters and reading fonts go on the SD card | formatted by the product on first use (this partition only) |
| `coredump` | Crash dumps | none |

The stock `model`, `resources` and `font_data` contents are overwritten by these regions; they are in the full backup.

## 4. Switching procedure (one-time, per device)

1. Build the probe with the new CSV; verify the generated `partition-table.bin` decodes to §3.
2. Write **only** `0x8000` (partition table) and the probe at the new `ota_0` (`0x40000`); bootloader and `otadata`
   untouched (otadata selects slot 0 by index, not by address).
3. Boot: the stock bootloader reads the new table and boots `ota_0`.
4. Probe checks: running partition `ota_0`; `nvs_flash_init()` on the product `nvs` (erase-on-first-use of that
   partition only); write/read a test key; `vendor_nvs` bytes unchanged (compare a read-back against the backup);
   coredump partition found; LittleFS format/mount on `assets`.
5. OTA check: write the probe to `ota_1`, select it, reboot, confirm it marks itself valid (rollback), select `ota_0`
   back.

Rollback to the stock layout: write the stock table bytes from the backup to `0x8000` (or restore the full backup).

## 5. Open questions

1. Does the device's v2.0.65 bootloader really have rollback enabled? (The probe already calls
   `esp_ota_mark_app_valid_cancel_rollback()`, which is harmless either way; step 5 shows it.)
2. `assets` filesystem: LittleFS needs the `joltwallet/littlefs` managed component (not in core IDF); FAT with wear
   levelling is in core IDF. LittleFS is the usual choice for power-loss safety.
