# 001 Survey of the official Metalio-E-INK4 firmware

- Surveyed: 2026-11-12
- Subject: <https://github.com/CloudZao/Metalio-E-INK4>, commit `d12f481f4a42420634bb56a928f3959a977367d4` (2026-10-13, "metalio-e-ink4 v2.0.63版本"). The repo has only this one commit, no tags and no releases; a `git fetch` on 2026-11-12 found nothing new. Devices ship with v2.0.65, for which neither source nor binary is public. Everything below describes the v2.0.63 source and **may differ from actual device behaviour**.
- Reading context: ESP-IDF 5.5.4, C/C++, LVGL 9.5.0, based on xiaozhi-esp32 (CMake project name `xiaozhi`), `main/` is about 106k lines. Source was only read; it was not built, flashed or run, and none of its scripts were executed.
- Evidence levels: `[source]` with `path:line` (relative to repo root) | `[community]` from README, docs, issues | `[inference]` our judgement. Nothing in this document is `[measured]` yet.
- License: root `LICENSE` is MIT (Copyright 2026 CloudZao). Third-party components in §15.

---

## 0. One-page summary

| Area | Official approach | For bunko |
|---|---|---|
| Display | SSD1677, SPI3 10 MHz. Ships three 105 B waveforms (fast full / partial / 4-gray) written via `0x32`. Each partial refresh sends the full 48 KB frame; after 15 consecutive partials a full refresh is forced | Driver and LUTs are the factual baseline for bring-up; the BUSY wait has no timeout and must be fixed |
| Grayscale | Only in immersive reading: 1-bit base first, then a 4-gray LUT pass adds an anti-aliasing layer, then written back to black/white | A ready-made path to learn from, but it is "gray layered over B/W", not true 4-gray layout |
| Touch/keys | CST816S polled every 10 ms. The three virtual keys on the cover are reported by the touch chip as **exact coordinates** (80/240/400, 900); plus BOOT/POWER/volume± | Virtual keys must match the stock coordinates exactly |
| Power | Light sleep only, no deep sleep. Wake sources are only the POWER key and a 60 s timer. Power-off = a 5 Hz square wave on TCA9555 P1.3, **in an infinite loop that never exits** | The power-off path is the prime suspect for issue #4 (§4.8); do not copy |
| Charging | CX25601N: VREG 4400 mV, ICHG default 1000 mA (README says 500), charger IC watchdog disabled, termination disabled whenever VBAT<4.4 V | Whether 4.4 V is within the cell rating is to-verify |
| Fuel gauge | BQ27220 used only for voltage and current. SOC is a linear map of 3.30–4.33 V; the chip's SOC is not used | Cause of the second symptom in issue #4; bunko needs a real gauge configuration |
| Audio | ESP32 is I²S **slave**, 16 kHz, 32-bit slots, stereo. Clock and codec live on an external Bluetooth module, switched between modes by UART AT commands. PA on P0.4 | Without `AT+RX=2`/`AT+MODE=1` there is no I²S clock and playback blocks forever |
| Reading | `.ebook`/`.epub`/`.txt`. Greedy per-code-point line breaking, **no kinsoku (禁則), vertical text or ruby**. Only UTF-8 and GBK, no Shift_JIS. Bundled fonts lack small kana | Japanese reading essentially has to be built ourselves |
| Study/dictionary | Does not exist | Entirely our own |
| Cloud | All 24 endpoint strings are blanked in the open-source tree; OTA uses the xiaozhi JSON protocol with no signature check | Cloud is unusable and should not be relied on |

---

## 1. Board, pins, partitions

### 1.1 GPIO (`main/boards/metalio-e-ink-4/config.h`) [source]

| Function | GPIO | Source |
|---|---|---|
| I²S mic WS / mic DIN / spk BCLK / spk DOUT | 43 / 17 / 6 / 7 (no MCLK) | `config.h:9-12` |
| Sample rate in/out | 16000 / 16000 | `config.h:6-7` |
| NT26 4G: RX / TX / MRDY / SRDY | 11 / 12 / 21 / 5 | `config.h:14-17` |
| Bluetooth module UART2 TX / RX | 48 / 47, 115200 | `config.h:19-20`, `periph/metalio_board_periph.cc:28` |
| BOOT / POWER | 0 / 3 (POWER goes to the "power on/off chip") | `config.h:22-23` |
| SDMMC CLK / CMD / D0 / DAT3 | 38 / 40 / 39 / 46 (46 is input-only with pull-up and must stay high) | `config.h:30-33` |
| USB D− / D+ | 19 / 20 | `config.h:35-36` |
| EPD MOSI / SCLK / CS / DC / RST / BUSY | 8 / 14 / 45 / 13 / 18 / 9, SPI3 10 MHz | `config.h:42-49` |
| I²C SDA / SCL | 41 / 42, all devices attached at 400 kHz | `config.h:51-52` |
| Touch INT / TCA9555 INT | 1 / 2 | `config.h:54-58` |
| Vibration motor | 44, active high, default pulse 35 ms | `config.h:61-62` |

### 1.2 TCA9555 @0x20 (`main/boards/common/bus/IOExpander.hpp:46-57`) [source]

| Pin | Name | Dir | Boot level (`bus/metalio_io_expander.cc:35-46`) | Notes |
|---|---|---|---|---|
| P0.0 | USB_MUX_SEL | out | 1 | low = camera, high = flashing/debug |
| P0.1 | PA_SWITCH | out | 0 | 1 = 4G call, 0 = ESP32 (`IOExpander.hpp:25`); nothing in the repo ever writes 1 |
| P0.4 | PA | out | 0 | Amplifier enable |
| P0.5 | SCREEN_SOCKET_PWR | out | 1 | Display socket power |
| P0.6 | MAIN_PWR | out | 1 | Main peripheral rail; powers the Bluetooth module (and 4G) [inference] |
| P0.7 / P1.0 | VOLUME_DOWN / UP | in | — | Active low |
| P1.1 | TOUCH_RST | out | — | Active low |
| P1.3 | PWR_KEY_PULSE | out | 1 (idle high) | Power-off pulse to the power-management chip |
| P1.4 | ACCEL_INT | in | — | Never read anywhere in the code |
| P0.2/0.3/1.2/1.5–1.7 | unmapped | | | |

- TCA9555 init probes up to 10 times, with an I²C bus reset between attempts (`IOExpander.hpp:316-336`). The GPIO2 interrupt pin is configured but the interrupt is never enabled (`metalio_io_expander.cc:55`). [source]
- Every per-pin `setLevel` at boot is wrapped in `ESP_ERROR_CHECK` (`metalio_io_expander.cc:35-46`); a single failed expander read/write aborts and reboots. [source]
- README points to `main/boards/common/IOExpander.hpp`; the file actually lives in the `bus/` subdirectory. [community]

### 1.3 I²C (I2C_NUM_0) [source]

- Internal pull-ups, glitch filter 7; 9 manual clock pulses for bus recovery before creating the bus (`bus/metalio_i2c_bus.cc:9-35,46-59`).
- Addresses: TCA9555 0x20, CST816S 0x15, PCF8563 0x51, BQ27220 0x55, CX25601N 0x6B, SC7A20H 0x19 (README §7.3 [community], consistent with the drivers).

### 1.4 Partition table (effective one is `partitions/v1/16m.csv`, selected by `sdkconfig:591-593`) [source]

| Name | Type | Offset | Size | Use |
|---|---|---|---|---|
| nvs | nvs | 0x9000 | 16K | |
| otadata | ota | 0xd000 | 8K | |
| phy_init | phy | 0xf000 | 4K | |
| model | spiffs | 0x10000 | 400K | esp-sr models (`application.cc:194`) |
| ota_0 / ota_1 | app | 0x80000 / 0x580000 | 5M ×2 | |
| resources | spiffs | 0xa80000 | 400K | UI resources; `ESP_ERROR_CHECK(mmap_assets_new)`, so a corrupt partition aborts at boot (`lv_adapter_display.cc:1527-1533`) |
| font_data | data 0x40 | 0xae4000 | 5M | UI fontpack, read via `esp_partition_mmap` (`font_loader.h:30`, `font_loader.c:151`); `use_font/font.fontpack` 4,985,431 B |
| coredump | coredump | 0xfe4000 | 64K | |

- There is no `assets` partition, so `assets.cc:49` does not find it and `CheckAssetsVersion` returns immediately (`application.cc:664-667`). `partitions/v2/*` is unused. [source]
- The table in README §9.5 matches the csv; README text says "Flash 16 MB, Octal PSRAM @80 MHz" (`sdkconfig:1386` `SPIRAM_MODE_OCT=y`). [community][source]
- There is no `sdkconfig.defaults`. `scripts/release.py:179` runs `idf.py set-target`; [inference] this regenerates sdkconfig, so release images do not necessarily carry the v1/16m table. A full-flash dump of a device is authoritative (to-verify).

### 1.5 Other key sdkconfig [source]

| Item | Value | Line |
|---|---|---|
| `CONFIG_PM_ENABLE` | y (DFS not auto-initialised) | 1365-1366 |
| Default CPU | 240 MHz | 1436 |
| Console | NONE, USB-Serial-JTAG as secondary console | 1505-1508 |
| Brownout | on, level 7, interrupt mode | 1287-1296 |
| Task WDT | on, 10 s, but `# CONFIG_ESP_TASK_WDT_PANIC is not set` (a hung task does not reboot) | 1514-1516 |
| INT WDT | 300 ms | 1511-1512 |
| Coredump | to flash, ELF format, checked at boot | 1620-1629 |
| App rollback | on (but see §12) | 422 |
| Secure boot / flash encryption | both off | 493, 3240 |
| RTC slow clock | internal RC | 1247 |
| `FREERTOS_HZ` | 100 | 1703 |
| ESP32 built-in BT | `# CONFIG_BT_ENABLED is not set` | 819 |
| FATFS code page | 936 (GBK) | 1668 |

---

## 2. Display (SSD1677 / GDEM0397T81)

### 2.1 Driver structure [source]

- Driver source: `main/boards/metalio-e-ink-4/display/esp_lcd_panel_ssd1677.c` + `esp_lcd_ssd1677_commands.h`, wrapped as an `esp_lcd_panel`; wiring in `metalio_epd.cc`.
- SPI: mode 0, no MISO, `trans_queue_depth=10`, `max_transfer_sz = 48000+8` (`metalio_epd.cc:21-41`).
- Framebuffer: 1 bpp, 800×480/8 = **48,000 B** (`commands.h:88-90`). `invert_color(true)` is called after init (`metalio_epd.cc:78`).
- The datasheet in the repo is `datasheet/DEPG0397BBS77AF11HP-2.1-GY.pdf`, not GDEM0397T81. Whether both are the same glass is to-verify.

### 2.2 Init sequence (`esp_lcd_panel_ssd1677.c:401-438`) [source]

1. `0x12` soft reset, delay 10 ms.
2. `0x18`=`0x80`, internal temperature sensor.
3. `0x37`, 10 bytes, 5th byte `0x40` (ping-pong option; the enclosing `#if SSD1677_EPD_OPT_PATH` is commented out, so it always runs).
4. `0x0C` booster soft-start: `AE C7 C3 C0 80` (`commands.h:50-61`).
5. `0x01` driver output: `DF 01 02`.
6. `0x3C`=`0x01` (border).
7. Load the fast LUT.
8. RAM window, data entry `0x11`=`0x01` (X increment, Y decrement, `commands.h:43`, `.c:115-142`).

### 2.3 Waveforms and refresh modes [source]

**Writing a LUT (`.c:348-372`)**: each table is 112 B, split as follows.
- `0x32` ← first 105 B
- `0x03` (gate voltage) ← `[105]`
- `0x04` (source voltage) ← `[106..108]`
- `0x2C` (VCOM) ← `[109]`

The driver caches the currently loaded table (`loaded_lut`) and only rewrites on a mode change; any reset clears the cache (`.c:200-217,344`).

| Table | File | VCOM byte | Notes |
|---|---|---|---|
| Fast/full | `main/display/LUT/epd_lut_fastupdate.h:12-31` | 0x50 | Source draft `LUT/fastupdate.txt` |
| Partial | `LUT/epd_lut_partial.h:12-31` | 0x00 | Frame-rate byte `0x22`, comment says "50hz" (`:52`). Differs from the source draft `partial.txt`: TP group 2 is 0x04 vs 0x3, VCOM 0x00 vs 0x8 (`:19,29` vs `partial.txt:7,17`) |
| 4-gray | `LUT/epd_lut_4gray.h:11-30` | 0x30 | Comment says "aligned with FreeInk/Cross lut_grayscale" |

**`0x22` Display Update Control 2 (`commands.h:72-86`, `.c:163-240`)**:

| Mode | 0x22 | Wait | Notes |
|---|---|---|---|
| FULL / STANDBY | 0xC7 | sync | Uses the MCU-written LUT; writes `0x3F`=`0x01` after refresh (`.c:227-232`) |
| PARTIAL | 0xCF | async: BUSY falling-edge ISR gives a semaphore, upper layer waits up to 5000 ms (`lv_adapter_display.cc:840-846`) | Border `0x3C`=`0x80` before refresh (`.c:565-575`) |
| GRAY | 0xCC | sync | External LUT, OTP not reloaded; forces `0x21`=`0x00` so the red plane takes part (`.c:187,205-208`) |
| FULL_FAST | 0xD7 + `0x1A`=`0x6A` written first (fake temperature) | — | **Dead code**, no callers; the `use_fast_full_update=true` flag (`metalio_epd.cc:48`) is not read by the driver either |

[inference] 0xD7 includes the "load LUT from OTP" bit. If FULL_FAST were enabled, the `loaded_lut` cache would no longer match reality.

### 2.4 BUSY and timing [source]

- **BUSY high = busy**. Polling is `while (gpio_get_level(busy)) vTaskDelay(1ms)` followed by a 100 µs wait (`.c:143-153`). **This wait has no timeout**; if the panel misbehaves it hangs forever, and since the Task WDT does not panic (§1.5) the result is a silent UI freeze.
- BUSY has a pull-down (`.c:297`); the falling-edge interrupt is only enabled before the partial `0x20` (`.c:236,306`).
- While BUSY is high, `draw_bitmap` returns `ESP_ERR_NOT_FINISHED` (`.c:444-446`). The upper layer then coalesces frames and resubmits on a 50 ms esp_timer (`lv_adapter_display.cc:1090-1107`).
- Refresh duration: no measured values in the code. Docs only say "full refresh takes a few seconds", referring to the 4-gray path (`docs/epd-bayer-dither-gray.md:16`) [community].

### 2.5 Partial / full policy and RAM planes [source]

- After `kPartialsPerFull = 15` consecutive partials, the next refresh is a full one (`lv_adapter_display.cc:618-619,1317`). Boot, entering/leaving standby and power-off all use full refresh (`:609,1136-1153,1790-1810`). Screens can call `RequestNextFullRefresh()`, mostly in the reader (`book_nav.cc:231`, `book_vk.cc:324`, `book_reader_body.cc:272`, `book_open_worker.cc:440`, `book_settings_sheet.cc:212`), `standby_screen.cc:38` and the burn-in test.
- Partial: **writes only the new frame to `0x24` (whole frame)**, relying on `0x26` still holding the previous frame (`:848-869`). The x/y/w/h parameters are ignored (`:852-856`). The dirty area is padded by 24 px (`kPartialDirtyPadPx`) and only used to decide "did anything change" (`:1352-1361`).
- Full: writes the new frame to both `0x26` and `0x24` (`:912-926`).
- Dual-plane partial (`RequestDualPlanePartial`) and the gray path explicitly write `0x26`=old, `0x24`=new (`:893-910`).
- **Panel deep sleep `0x10`=`0x01` is never called**. The call in the power-off task is commented out as "temporarily disabled… for bring-up debugging" (`power_hw.cc:367-374`); standby only freezes flushing and does not put the panel to sleep (`lv_adapter_display.cc:638`).

### 2.6 Grayscale [source]

- **Reading anti-aliasing (only in immersive reading, `book_geom.cc:21-28`)**:
  - LVGL draws glyphs as solid I1 as usual. A patched hook (`lv_draw_sw_blend_to_i1/`, `epd_i1_glyph_thin.h:33-43`) writes A2 coverage (0/85/170/255) into a 384 KB L8 side buffer in PSRAM (`epd_gray_aa.cc:40-66`).
  - Flush in five steps (`lv_adapter_display.cc:929-998`):
    1. Refresh the B/W base (partial or full).
    2. Coverage <128 maps to light gray (MSB plane only), 128–254 to dark gray (both planes) (`epd_gray_aa.cc:92-119`).
    3. LSB written to `0x24`, MSB to `0x26`.
    4. Refresh with the 4-gray LUT, `0x22`=`0xCC`.
    5. Write the B/W frame back to both planes.
  - The LVGL patch must be reapplied by hand after dependency updates (`lv_draw_sw_blend_to_i1/README.md:1-12`).
- **Home-screen card gray**: 4×4 Bayer dither, threshold 2 (`home_screen_priv.h:47-53`, `home_card_style.cc:9-39`). The docs name a different file (`docs/epd-bayer-dither-gray.md:7`).
- **Images**: PNG (lodepng), JPEG and A2I1 are all converted to L8 first, then 4×4 Bayer-dithered to 1 bit (`reader/image_util.cc:12-33,210-253`).

### 2.7 Orientation and pixel format [source]

- LVGL I1 HTILED, 1 = white (`lv_adapter_display.cc:603-604`). The adapter is fixed at ROTATE_0; rotation is done in software at flush time (`:1491-1501`, `display_orient.cc:28-39`):
  - Portrait (logical 480×800): `px=ly, py=ph-1-lx`
  - LandLeft: identity
  - LandRight: 180° rotation
- A2I1 image format: 12 B header (magic/w/h/stride/reserved) + 8 B palette + 1 bpp bitmap (`lv_adapter_display.cc:60-94`). PC-side converter in `tools/a2i1/`.

---

## 3. UI (LVGL) and input

### 3.1 LVGL [source]

- Versions: `lvgl/lvgl ==9.5.0`, `espressif/esp_lvgl_adapter ==0.6.3` (`main/idf_component.yml:44-46`); `CONFIG_LV_COLOR_DEPTH_1=y` (`sdkconfig:2836`).
- LVGL task stack 16 KB in PSRAM, priority 1, on core 1. Uses `SPI_MONO_DEFAULT_CONFIG` with `use_psram=true`. The `draw_bitmap` callback is replaced with a custom `EpdLvglDrawBitmap`; the area rounder pads ±24 px (`lv_adapter_display.cc:1479-1514`).
- Three 48 KB buffers in PSRAM: `last_fb`, `work_fb`, `gray_scratch` (`:1459-1467`). The driver copies into an internal DMA bounce buffer when sending (`.c:480-496`).
- Flush flow: LVGL dirty area → `OverlayDirty` (which also does the rotation) into `work_fb` → choose FULL / GRAY / PARTIAL (`:1161-1385`).
- Locking via `esp_lv_adapter_lock` (`:2313-2318`); screen switches go through `lv_async_call` (`screen_common_nav.cc:91-135`).
- There is no "refresh mode per screen" table. Default is partial, full every 15; screens can request a full or dual-plane partial.

### 3.2 Application model [source]

- Home `kApps[]` has six apps: To-do (Task), Baiwen AI (Assistant), Reading (Book), Wallpaper, Transfer (Cloud), Settings (`home_screen.cc:42-49`). Grid 3×4, 12 per page (`home_screen_priv.h:41-44`).
- Navigation: `ScreenNavigateTo` pushes the current screen's factory onto a back stack (depth 8, `screen_common_priv.h:12`), then replaces the screen; the old screen is deleted asynchronously (`screen_common_nav.cc:24-53`). Back = pop and rebuild the previous screen (`screen_common.cc:93-103`). Key routing has a separate VK screen stack, also depth 8 (`vk_key_handler.cc:21-73`).
- `ScreenRequestBack()` actually goes Home, not to the previous screen (`screen_common_nav.cc:87-89`).
- "No scrolling" is a hard convention (`README_zn.md:46,59` [community]): about 41 files clear `LV_OBJ_FLAG_SCROLLABLE`. Exceptions are the Bluetooth list (`bluetooth_screen.cc:372`) and the battery test page (`settings_test_battery_screen.cc:421`).
- System pages: standby, OTA upgrade, `touch_missing_screen`, Bluetooth page embedded in Settings.
- **A2UI**: a server-pushed JSON→LVGL renderer supporting Text / RichText / Math (LaTeX subset, Latin Modern Math font) / Image (A2I1). The device paginates by glyph metrics, up to about 1500 pages (`main/display/a2ui/README.md`, README §11.2) [community].

### 3.3 Touch CST816S [source]

- Uses the library's default I²C address (0x15), 400 kHz; INT=GPIO1; RST on TCA9555 P1.1 (bit 9) (`metalio_touch.c:17,58,67`). Native coordinates 480×800, no swap or mirror (`:80-97`).
- **Polling**: the `touch_feed` task reads every 10 ms, priority 5, on core 0 (`lv_adapter_display.cc:194-196,450-486`); the INT pin is configured but unused. Press/release edges go into a depth-50 queue so taps are not lost while LVGL is busy (`:197-213,502-599`).
- No gestures.
- Sleep: write `0xA5`=`0x03`; woken by hardware reset (`metalio_touch.c:118-145`).

### 3.4 Cover virtual keys [source]

| Key | Coordinate (x, y) | Default action (`vk_key_handler.cc:130-156,283-318`) |
|---|---|---|
| HOME | (80, 900) | Go Home; long press also Home; in Baiwen, long press = PTT |
| NEXT | (240, 900), comment "cover middle" | Does nothing by default; taken over by screens (next page) |
| PREV | (400, 900), comment "cover right" | Back by default; taken over by screens (previous page) |

On the device the source comments' positions are swapped: the **middle** pill reports (400, 900) and the **right**
pill reports (240, 900) `[measured]` (research/003 P3).

- Coordinates defined in `config.h:64-70`. y=900 is outside the 800 px panel; it is a fixed point the touch chip reports for the cover key area.
- **Matching requires exact coordinate equality**: `x == vk.x && y == vk.y` (`lv_adapter_display.cc:219-226`).
- Long-press threshold 500 ms (`metalio_display_setup.cc:48-54`). Holding PREV/NEXT jumps ±10 pages per second from the press (`vk_page_repeat.cc:12-15,44-53,102-104`).
- Note the physical position vs key name: middle is NEXT, right is PREV.
- `vk_key_handler.cc` is not valid UTF-8; comments are garbled (`:269,292,306,310`).

### 3.5 Physical keys (`main/boards/metalio-e-ink-4/input/metalio_buttons.cc`) [source]

| Key | Behaviour |
|---|---|
| BOOT (GPIO0) | Single/double click / long press 500 ms, dispatched per screen (`boot_key_handler.cc:44-174`); long press in standby opens Baiwen |
| POWER (GPIO3) | Short press enters/leaves standby; long press 3000 ms powers off (`power_policy.h:23`, `metalio_buttons.cc:96-144`) |
| VOL− (P0.7) / VOL+ (P1.0) | Read by `iot_button` polling the expander (`:26-61`). While reading, VOL+ acts as `vk_prev` and VOL− as `vk_next`, with long-press repeat (`:154-175,199-201,269-271`); otherwise volume ±10 (`:176-183`) |

All keys are masked during OTA, and also during the under-voltage lockout (`metalio_buttons.cc:100`).

---

## 4. Power

### 4.1 Sleep modes [source]

- **Light sleep only**: the only call site is `esp_light_sleep_start` (`power_policy/power_hw.cc:437`). There is no `esp_deep_sleep_start` anywhere in `main/`. Automatic light sleep is also off, CPU frequency min=max (`power_hw.cc:327-332`).
- **Wake sources** (`power_hw.cc:405-424`):
  - All wake sources are disabled first; then only **POWER key (GPIO3) low level** and the **timer** are enabled.
  - BOOT is explicitly disabled (`:407`); touch goes into deep sleep. Neither the RTC alarm nor the accelerometer interrupt is a wake source.
  - Timer slices are at most 60 s (`power_policy_priv.h:31`). Even with "never power off" set it still wakes every 60 s (`power_policy_standby_lp.cc:233-236`).
- Before sleeping, EPD_RST is held high with `gpio_hold_en` (`power_hw.cc:105-112`); the charger VREG task is suspended during sleep (`:436,438`).
- In standby, BOOT is only **polled** on timer wakes, so a short BOOT press does not wake the device. The comment at `power_policy_standby_lp.cc:79` claims BOOT can wake it, contradicting `power_hw.cc:407`.

### 4.2 Policy levels (`power_policy_priv.h:20-31`, `power_policy.cc`) [source]

| Level | Contents |
|---|---|
| Full | Phone/BT: full clock, PA on |
| NetActive | Full clock, Wi-Fi kept |
| KeepNet | Reserved, unused |
| AppIdle | PA off, Wi-Fi off (`esp_wifi_stop`, netif kept), CPU down to `idle_mhz` (80/160/240, default 240) |
| StandbyUi | Standby screen + 60 s light-sleep slices; **MAIN_PWR is turned off only at this level** (`power_policy.cc:592`) |

- **Default timings (code)**: 3 min idle → standby; Wi-Fi kept 30 s before disconnect; **power off after 30 min cumulative standby** (`power_policy_priv.h:28-30`). README §2.5 says power off 3 min after standby [community], which disagrees with the code.
- NVS namespace `power`, keys `idle_mhz`, `idle_stb_s`, `net_grace_s`, `stb_off_s`. `stb_off` can be 0 (never)/3m/10m/30m/3h/6h (`power_policy.cc:57-62,334,351-352`).
- A 1 Hz esp_timer `pwr_pol` re-evaluates the level, checks the standby power-off and calls the under-voltage check (`:433-446,770-778`).
- The virtual USB disk and `StandbyInhibit` block entering standby (`:179-181`).
- **Policy ignores VBUS and charging state**: plugging/unplugging USB changes no power behaviour.

### 4.3 Standby flow (`power_policy_standby_lp.cc`) [source]

1. Touch into deep sleep (`:415`), `iot_button_stop` (`:419`).
2. Wait up to 2 s for the standby screen to finish refreshing, then park the EPD (`:422-427`).
3. Loop over 60 s light-sleep slices (`:359-384`).
4. POWER short press leaves standby; long press ≥3 s powers off; BOOT long press ≥500 ms opens Baiwen (`:84-127`).

Two standby screens: classic (date, Chinese lunar calendar, weather, to-dos) and full-screen wallpaper (A2I1) (README §11.4) [community].

### 4.4 Power-off (`power_hw.cc:342-399`) [source]

```c
// PwrOffTask (power_hw.cc:361-385)
PrepareForPowerOff();               // PA off; MAIN_PWR and SCREEN_SOCKET_PWR stay on
disp->ShowPoweredOffScreen();
// Temporarily disabled: settle after power-off screen / Deep Sleep / pin park; send pulses directly for bring-up debugging
constexpr int kPulseHalfMs = 100;
for (;;) {                           // 5 Hz square wave, never exits
    io.setLevel(PWR_KEY_PULSE, true);  vTaskDelay(100ms);
    io.setLevel(PWR_KEY_PULSE, false); vTaskDelay(100ms);
}
```

- The loop has no timeout, does not check VBUS and never calls `esp_restart`. The design intent is [inference]: the power IC cuts power on receiving the pulse and the ESP loses power with it, so no exit is needed.
- `BeginPowerOffLocked` sets `shutdown_requested=true` and **never clears it** (`power_policy.cc:548-560`). From then on every power-off request is a no-op and the standby loop also stops (`power_policy_standby_lp.cc:343-345`). `power_hw_begin_power_off` has an additional `static bool started` gate (`power_hw.cc:389-393`).
- Touch is put into deep sleep before power-off (`power_policy.cc:554`).

### 4.5 Charging (CX25601N @0x6B, `main/boards/common/power/cx25601n.cc`) [source]

| Item | Value | Source |
|---|---|---|
| D+/D− auto-detect | off, so VBUS_STAT often reads 0 even with USB plugged in | `:353`; `docs/battery-soc-and-charge.md:125,155` |
| HIZ | 0 | `:356` |
| **Charger IC watchdog** | **off** (REG0x16 WDT=0) | `:357` |
| IPRECHG / ITERM | 240 mA / 60 mA | `:360,364` |
| **VREG** | **4400 mV** | `:368` |
| ICHG | Driver default writes 500 mA; the board then overrides from NVS `charge/ichg_ma`, **default 1000** (only 500/1000 allowed) | `:371`; `periph/metalio_board_periph.cc:110-115` |
| IINDPM | Set to the same value whenever ICHG is written | `:649-652` |

`charger_vreg_task` (`:180-236`) polls every 2 s while VBAT is below the cutoff, otherwise every 10 s:
- `set_en_term(!vbat_below)`: **charge termination is disabled while VBAT<4400**; only once reached may charging stop at ITERM (`:229`).
- If charging is idle, current <5 mA and VBAT<VREG, it kicks a recharge (`:232-234`).
- Under-voltage: when VBAT ≤3100 mV and VBUS_STAT==0, writes REG0x18=0x01 to turn off BATFET (`:46,212-221`).

Charging strategy in the docs: `docs/battery-soc-and-charge.md` §4 [community].

### 4.6 Fuel gauge (BQ27220 @0x55, `bq27220_gauge.cc`) [source]

- Reads only Voltage (0x08) and Current (0x0C) (`:11-12`), **chip SOC unused**; the docs' reason is "uncalibrated, prone to distortion" (`docs/battery-soc-and-charge.md` §3) [community].
- Algorithm:
  - Linear map 3.30 V → 0%, 4.33 V → 100% (`:19-20`).
  - 60-point moving average gives the target. The displayed value moves at most 1% per 30 s, only up while charging and only down while discharging.
  - Reaching 100% needs ≥4330 mV held for 30 s; once 100% is shown it only leaves when dropping below 4280 mV (`bq27220_gauge.h:62-66`).
  - Charging flag = current >5 mA and displayed value <100 (`:176,194`).
- [inference] The first reading at boot becomes the displayed value directly, then it steps slowly and monotonically. Rebooting under different load or plug state therefore gives very different initial values. This matches the "100→65→99→82" in the second symptom of issue #4.
- No chemistry parameters such as Design Capacity are written to the gauge and no learning is done (the repo only ever reads the BQ27220).

### 4.7 Software under-voltage (`battery_uv_shutdown.cc`) [source]

- When VBAT <3200 mV and current ≤5 mA, show a low-battery screen for 2 s, then `RequestPowerOff` (`:21,30-47`).
- **This check is skipped during standby** (`:71-73`) and during OTA (`:74`).
- Once triggered, `BatteryUvLockout_IsActive` masks all keys.
- [inference] FreeRTOS ticks do not advance in light sleep, so tasks driven by `vTaskDelay` (including the 3.1 V BATFET protection in the charger task) barely run in standby.

### 4.8 Issue #4 "unresponsive after charging" cross-check

**User report** (factory v2.0.65 unit, 2026-11-11) [community]:
1. Charged overnight from a computer USB port, no network during that time. After unplugging, long-pressing POWER did nothing.
2. Left unplugged overnight, plugged into USB again and long-pressed 10 s: still nothing.
3. Unplugged USB, waited about 1 min, long-pressed 10 s again: booted normally.
4. The coredump partition was empty.

**Matching paths in the code**:

| # | Path | Basis | Assessment |
|---|---|---|---|
| A | Plugged in and idle: standby after 3 min, auto power-off after 30 min cumulative, entering `PwrOffTask`. While USB supplies power, the power IC may be unable to cut system power, so the ESP keeps emitting the 5 Hz square wave. At that point: screen frozen on "powered off", touch asleep, `shutdown_requested` stuck, all key paths are no-ops, and the watchdog does not panic | §4.2, §4.4, `power_policy_priv.h:30`, `power_hw.cc:379-384`, `sdkconfig:1516` | [inference] Prime suspect. Fits "long press does nothing with USB in; works after unplugging and waiting": the power IC's latch/discharge needs VBUS gone for a while |
| B | The continuous square wave may be read by the power IC as repeated key presses, toggling on/off repeatedly, with the final state depending on when the cable is pulled | Power IC model and timing not public | [inference] to-verify |
| C | BATFET turn-off: the criterion is VBUS_STAT==0, but D+/D− detection is off, so it may read 0 while plugged in. A deeply discharged device could have BATFET turned off while plugged in | `cx25601n.cc:212-221,353` | [inference] No sign of deep discharge in this case; unlikely |
| D | Empty coredump ≠ no panic: at boot, if the SD card is mounted and logging is on (default on), the flash coredump is **moved to SD `metalio/e-ink/logs/metalio_coredump.elf` and the slot erased** | `application.cc:1200-1202`, `sd_log_writer.cc:351-440` | [source] That file on the SD card should be checked |

Recovery: not officially documented. Going by the code, "unplug USB → wait for the power IC to discharge → long-press POWER" works.

**Evidence from the device's own log** `[measured]` (2026-11-18, `metalio/e-ink/logs/metalio_logs.txt` read off the SD card;
the RTC was never set, so dates are wrong but the order is reliable). The last events written before the log wraps:

```
PowerPolicy: standby LP idle elapsed=1800 s (limit 1800 s) -> power off
PowerHw: begin power off (UI + pulse)
PowerHw: PWR_KEY_PULSE
HeapCheck: OK: internal RAM + PSRAM integrity        <- ~2 s later, the ESP32 is still running
```

Interpretation, revised after P7 (research/003): on this unit, P1.3 pulses cut power within ~2 s **even with USB
attached**, and the log ends right after the line written ~1.9 s after the first pulse. So the log is consistent with a
**successful automatic power-off** after 30 minutes of standby, not with a firmware stuck in `PwrOffTask`
`[inference]`. What made the user's power-on attempts fail that night (a 3 s press boots the unit after the same kind of
power-off in our test) remains open. The same log shows the charger "recharge kick" running every ~2.7 s
(`vbat=4278mV < VREG=4400mV`, status "not charging / full" each time) while the UI showed 95 %.

A separate core dump on the card (`metalio_coredump.elf`, ESP-IDF core dump with a 24-byte header) records a crash with
EXCCAUSE 20 (InstFetchProhibited), EXCVADDR `0xad567820` (garbage), EPC1 `0x4037bc23`; the only task name present is
`uart_rx_task` (the BT audio module's UART receiver). Its time is unknown, and without the v2.0.65 ELF the stack cannot be
symbolised.

---

## 5. Storage

### 5.1 NVS [source]

- Namespaces: `power`, `charge` (`ichg_ma`), `display` (`haptic`/`haptic_lvl`/`sd_log`), `network`, `ui` (language), `chat_cloud`, `wifi`, `vendor`, `audio` (`output_volume`), `assets`, `board`, `ota` (`skip_ver`).
- When `nvs_flash_init` returns NO_FREE_PAGES or NEW_VERSION, **the whole NVS is erased** (`main/main.cc:22-27`); this is unmodified xiaozhi code.
- `SystemReset` (erase NVS + otadata + reboot) is implemented (`boards/common/board/system_reset.cc:26-72`) but never instantiated; the UI has no factory reset.

### 5.2 SD card (`main/boards/common/storage/SdCardManager.hpp`) [source]

- SDMMC 1-bit (`:234`), only CLK/CMD/D0 connected, internal pull-ups (`:226-235`). DAT3 (GPIO46) is input-only with pull-up to keep the card from entering SPI mode (`periph/metalio_board_periph.cc:58-65`). Frequency from `SDMMC_HOST_DEFAULT()` ([inference] 20 MHz).
- FAT mounted at `/sdcard`: `format_if_mount_failed=false`, `max_files=12`, allocation unit 16K (`:46-58`). Long file names on (heap, 255 chars, `sdkconfig:1642,1669`); exFAT off (no `FATFS_EXFAT` entry in sdkconfig).
- All directories under `/sdcard/metalio/e-ink/` (`sd_paths.h:21,40-65`): `books`, `fonts` (`.ef`), `fonts_ttf`, `wallpaper` (incl. `bg_shutdown.a2i1`), `recordings`, `chat_log`, `a2ui_cache` (the last two cleared at boot), `logs` (`metalio_logs.txt` 256 KB ring + `metalio_coredump.elf`).
- SD logging (`sd_log_writer.cc`):
  - Hooks `esp_log_set_vprintf`, buffers into a 12 KB RAM ring, flushed to disk about once per second (`:33-34,280-293,333`).
  - Toggle in NVS `display/sd_log`, default on (`:29-31`).
  - [inference] Logs still in the ring at the moment of power loss are lost.
- SD firmware update: if `/sdcard/Metalio-E-Ink4.bin` (`main/ota.h:13`) is found at boot, a confirmation dialog is shown; the file is deleted after updating (`application.cc:835-908`).

### 5.3 USB virtual disk (`main/boards/common/storage/usb_virtual_disk.cc`) [source]

- Off by default; the PHY belongs to USB-Serial-JTAG (`:144`, `RoutePhyToUsj` `:211-218`).
- When enabled in Settings → Storage: the PHY switches to OTG (`:195-205`) and TinyUSB MSC exports the raw SDMMC card (`:408-440,532-557`). While exported, the device unmounts the SD and stops writing logs (`SdCardManager.hpp:100-158,186-189`).
- Mutually exclusive with USB-Serial-JTAG. Leaving the Settings page forces it off (`:766-781`). Holds `PowerNeed::UsbVirtualDisk`, blocking standby (`:57-68`). USB strings are `Metalio` / `Metalio Ink SD` (README §14.5) [community].
- **The virtual disk does not touch the USB mux P0.0**: P0.0 is only pulled low by the UVC camera code (`camera/usb_uvc_still.cc:163`); it boots high (flashing/debug path).

### 5.4 Internal FS

- Two SPIFFS partitions, `model` (esp-sr models) and `resources` (UI resources, mmap), plus the raw `font_data`.
- **There is no writable internal filesystem on the device**; all user data is on SD.
- Writing a fontpack from SD into `font_data` is marked "not implemented" in the design doc (`docs/sd-font-resources-upgrade.md:34`) [community]. The factory writes it with `tools/fontpack/flash_fontpack.sh` calling `parttool.py` (`:16-30`) [source].

---

## 6. Wi-Fi

- The component is vendored `78/esp-wifi-connect v2.6.2`, MIT (`components/esp-wifi-connect/idf_component.yml:1-6`). [source]
- **Provisioning SoftAP** (`wifi_configuration_ap.cc`) [source]:
  - SSID `MetalioEInk4-%02X%02X`, from the last two MAC bytes (`:43,126`). Open auth, up to 4 clients, address 192.168.4.1 (`:132-169`).
  - DNS hijack, plus 302 redirects for probe paths such as `/hotspot-detect.html`, `/generate_204*`, `/ncsi.txt` (captive portal, `:155,485-510`).
  - Web endpoints `/`, `/scan`, `/submit`, `/saved/*`, `/reboot`, `/advanced/*` (`:241-561`). The advanced page can write NVS `ota_url`, `max_tx_power`, `remember_bssid`, `sleep_mode` (`:610-671`).
  - `WIFI_PS_NONE` while the provisioning AP is up (`:174`).
  - There is also an on-device password keyboard page (issue #1 is a PR about it) [community].
- **STA**: when no saved AP is found it rescans every 10 s, up to 5 reconnect attempts (`wifi_station.cc:17-19`). A `SetPowerSaveMode` (MIN_MODEM) capability exists but `main/` never calls it; power saving relies entirely on `esp_wifi_stop` in AppIdle/standby. [source]
- **Wi-Fi location upload** (`main/device_wifi_location.cc`) [source]: scans after boot, takes the top 10 APs by RSSI and POSTs `{ssid, BSSID, signal}` to `DeviceLocationWifiUrl()` (`:33,321-386`). Skipped when the endpoint is empty (`:378-380`, `application.cc:1298-1305`).

---

## 7. Bluetooth and audio

### 7.1 External Bluetooth audio module (UART AT) [source]

- Transport: `SimpleUart` singleton, 1 KB each for RX/TX, RX task polls every 100 ms and splits lines on `\n` (`SimpleUart.hpp:25-66,143-158`, `bluetooth_uart.cc:162-183`).
- **The module model is not named anywhere in code or docs** (grep for Jieli/AC69/BK32 etc. finds nothing).
- The ESP32's own BT/BLE stack is fully disabled (`sdkconfig:819`).

| Purpose | Command (all terminated by `\r\n`) | Source |
|---|---|---|
| Mode 1 (conversation, boot default) | `AT+RX=2`, wait 700 ms, `AT+MODE=1` | `bluetooth_uart.cc:192-196`, at boot via `bluetooth_screen.cc:405-408` |
| Mode 2 (transmitter to a headset; headset must have a mic) | `AT+TX=1`, wait 700 ms, `AT+MODE=2` | `:200-204` |
| Mode 3 (music receiver, device acts as speaker) | `AT+RX=1`, wait 700 ms, `AT+MODE=3` | `:208-212` |
| Call | `AT+PP=1`, wait 200 ms, `AT+BTSCO=1` | `:253-256` |
| Music | `AT+BTSCO=0`, wait 200 ms, `AT+PP=1` | `:265-268` |
| Scan (mode 2 only) / connect | `AT+INQUIRING` / `AT+CONNECT=<addr>` | `bluetooth_screen.cc:133,237,245` |

- Parsed responses (substring match, `bluetooth_uart.cc:44-159`): `SET MODE n`, `RECONNECT`, `INQUIRING START`, `AT+BT:<hexaddr><name>`, `INQ COMPLETE`, `CONNECTING`, `CONNECT SUCCESS`/`TIMEOUT`, `SETUP SCO`, `DISC SCO`.
- After MAIN_PWR is dropped and restored, it waits `kMainRailBtSettleMs = 400` ms, then re-sends mode 1 (`power_hw.cc:33,57-62`).
- [inference] `registerCallback` replaces a `std::function` from the UI task while the RX task may be invoking it, with no lock: a data race (`SimpleUart.hpp:72-74,147-150`).

### 7.2 I²S (`main/boards/common/audio/bt_audio_codec.cc`) [source]

| Item | Value | Source |
|---|---|---|
| Port / role | I2S_NUM_0, **SLAVE** (BCLK/WS from the Bluetooth module) | `:31-33` |
| DMA | 6 × 240 frames (about 15 ms per descriptor, [inference] about 90 ms total) | `audio/audio_codec.h:14-15` |
| Format | Philips std, 32-bit data, 32-bit WS, stereo BOTH, bit_shift, mclk×256 | `:42-52` |
| TX/RX | Same `std_cfg` (output sample rate), full duplex sharing BCLK/WS | `:44,61-62` |
| Write | Mono int16 × `(vol/100)²×65536` → int32, written to both L and R. Pure software volume, NVS `audio/output_volume`, minimum 10 | `:66-103`; `audio_codec.cc:30-35,50-56` |
| Read | 32-bit samples `>>12` then clamped to int16; two channels, ch0 = mic, ch1 = reference | `:105-124`; `audio_service.cc:272-284` |
| Blocking | `i2s_channel_read/write(..., portMAX_DELAY)`; write path wrapped in `ESP_ERROR_CHECK` | `:100,110` |

- **Output path**: ESP DOUT(7) → Bluetooth module → PA_SWITCH (P0.1=0 selects ESP32) → PA (P0.4) → speaker (README §2.5/§5 [community]).
- **Mic**: [inference] on the Bluetooth module side, coming in via DIN(17). There is no schematic in the repo (`datasheet/` only has two PDFs, EPD and TCA9555).
- [inference] When the module is not in a mode that outputs I²S clocks, the `portMAX_DELAY` reads/writes block the audio task forever.

### 7.3 Playback / recording start and stop [source]

- `AudioCodec::Start` enables both I²S channels (`audio_codec.cc:29-48`). On this board `EnableInput/Output` only flip flags and do not touch hardware (`:63-77`). After 15 s idle (`AUDIO_POWER_TIMEOUT_MS`) the flags are cleared (`audio_service.h:41`, `audio_service.cc:785-797`).
- **PA is controlled only by PowerPolicy**:
  - `DeviceWantsPa` is true only while Speaking or AudioTesting; the Full level (PhoneCall/BtAudio) also enables it (`power_policy.cc:133-142,607-612,640-651`).
  - [inference] There is no explicit anti-pop/settling delay between PA enable and the first samples.
- **Opus** (unmodified xiaozhi): decodes mono 60 ms frames; encodes 16 kHz, mono, 60 ms, complexity 0; send/receive queues 40 packets each (`audio_service.h:32-37`, `audio_service.cc:47-49`). When the server sample rate differs, the decoder is rebuilt and resampled to 16 kHz (`:500-516`).
- **AFE**: enabled (`CONFIG_USE_AUDIO_PROCESSOR=y`), type VC, input format "MR", VAD on, NS on when a model is present, AGC off (`afe_audio_processor.cc:28-72`). Both device AEC and server AEC are off (`sdkconfig:651-652`); the reference channel is captured but unused.
- **Wake word**: enabled in Kconfig (WN9 "你好小智", `sdkconfig:647,698`), but at runtime `wake_word_.reset()` with the comment "this product does not create a wake-word object" (`audio_service.cc:798-806`). Actual interaction is push-to-talk (PTT); holding about 500 ms starts capture (README §11.2).
- **AFSK acoustic provisioning**: dead code (`afsk_demod.h:13-22`, `sdkconfig:655`).
- **Recorder app**: README says the `recordings/` directory is used by a "Recording app" (§11.7) [community], but there is no such app in the home `kApps[]`.
- **Latency**: no measured values in the code; the I²S part is about 90 ms DMA + 60 ms frames [inference].

---

## 8. Haptics, RTC, accelerometer, microphone

| Peripheral | Official usage | Source |
|---|---|---|
| Motor GPIO44 | Plain push-pull, no PWM; one-shot esp_timer turns it off; has a continuous mode; `gpio_sleep_sel_dis` keeps the level during sleep. Strength weak/medium/strong = 35/50/70 ms, default weak; NVS `display/haptic`, `haptic_lvl` | `input/metalio_vibe.cc:22-74`; `main/haptic_feedback.cc:15-25` |
| PCF8563 @0x51 | Init only clears CTRL1 to start the oscillator. CTRL2, alarm and timer are not configured, **not a wake source**. At boot the RTC time is loaded into the system clock (the VL low-voltage flag is only logged; the time is used anyway). After connecting, the `server_time` from the OTA response is written back to the RTC | `sensor/pcf8563.cc:58-61,150`; `periph/metalio_board_periph.cc:89`; `application.cc:1285-1287` → `metalio_e_ink_4_board.cc:113-128` |
| SC7A20H @0x19 | 100 Hz, ±2 g, **only read by the factory test page**. No orientation/rotation, no raise-to-wake; ACCEL_INT not read | `sensor/sc7a20h.cc:18-23`; `settings_test_sensors.cc:119` |
| Microphone | See §7.2; on the Bluetooth module side [inference] | |

Issue #3 asks "can it wake on a timer to refresh the wallpaper" and "after power-off, is a long press the only way to turn it on"; no official reply [community]. Current code [inference]: the timer can wake light sleep, but no timed-refresh feature exists; whether an RTC alarm can wake the power IC after power-off depends on the schematic (to-verify).

---

## 9. 4G (NT26) and camera

- **4G**:
  - The board class derives from `DualNetworkBoard(TX,RX,MRDY,SRDY,0)`, Wi-Fi by default; NVS `network/type`=1 switches to 4G, with a reboot on switch (`metalio_e_ink_4_board.cc:46`, `dual_network_board.cc:41-81`). [source]
  - The link is UART1 at **2 Mbaud**, UHCI DMA framing plus MRDY/SRDY handshake, with NAT on the module side to 192.168.10.2 (`nt26_board.cc:173-180`, `uart_eth_modem.cc:281-1996`). [source]
  - No dedicated power or reset pin (`nt26_board.h:74` `reset_pin=NC`); powered with MAIN_PWR. [source]
  - README says "Networking is Wi-Fi" [community], but the 4G code is complete and reachable.
  - 4G call audio routing (PA_SWITCH=1) is not implemented.
- **Camera**:
  - USB UVC host. When taking a photo it pulls P0.0 low, waits 80 ms for the mux to settle, switches the USB-Serial/JTAG pads to the OTG PHY, and switches back afterwards (`usb_uvc_still.cc:35,163-205,512`).
  - Takes the first MJPEG format, frame buffer 40–280 KB, timeout 20 s (`usb_uvc_still.cc:32-46,438-450`, `uvc_still_camera.cc:17`).
  - Only used for Baiwen photo upload (`VisionUploadUrl`); the MCP photo tool is disabled with `#if 0` (`mcp_server.cc:99-122`). [source]

---

## 10. Reading

### 10.1 Formats [source][community]

- `.ebook`, `.epub`, `.txt`. The device also writes these side files:
  - `.txt.idx` (v9)
  - `.pos` (`POS3`, 28 B: chapter, page, per-mille, reading seconds, today's reading seconds)
  - `.cpg` (per-chapter page-count cache, `CPG1`, keyed by font fingerprint)
  
  Source: `book_session.cc:58,586-636,2437-2470,974-993`.
- **`.ebook` v1** (`tools/ebook/SPEC.md`):
  - 64 B header, magic `EBOK`, little-endian, CRC32 over the first 56 B (`:65-86`).
  - flags: HAS_COVER / IMG_A2I1 / ZLIB / HAS_TOC / PAGE_IMAGES (`:88-97`).
  - Chapter index entries are 32 B each. Data blocks have an 8 B header, type TEXT/IMAGE, each block zlib'd separately, uncompressed block limit 4096 B.
  - Text uses `0x1E` as paragraph separator and `0x0A` as soft line break; images can be JPEG/PNG/A2I1/raw grayscale (`:23-29,122-185`).
  - Firmware rejects chapters over 384 KB; the converter splits chapters over 256 KB (`book_session.cc:65`).
- PC converter `tools/ebook/convert_ebook.py`: PDF/EPUB/MOBI/TXT → `.ebook`, with a web preview and simulator (README §2.2) [community].
- EPUB: tags are simply stripped (`html_content.cc:233-294`); HTML entities recognised are only amp/lt/gt/quot/apos/nbsp and numeric entities (`:128-160`). [inference] NCX/nav is not parsed; chapters are spine items.

### 10.2 Layout [source]

- **Greedy per-code-point line breaking**: wrap when `line_w + glyph_w > viewport_w` (`book_session.cc:2088-2097` for epub/ebook, `:1664-1670` for txt). One LVGL label per line (`reader_page_render.h:124-133`).
- **None of the following exists** (grep finds nothing): kinsoku (line-start/line-end punctuation rules), word-based wrapping for Latin text, hyphenation, **vertical text**, **ruby/furigana**. [inference] `<rt>` in EPUB is shown inline as body text.
- First-line indent is the width of two "一" (`:113-126`). Bold chapter titles are faked by drawing twice with a 1 px offset (`reader_page_render.h:108-117`).
- No kerning (`letter_next=0`); missing glyphs count as 1 px wide (`book_session.cc:105-106`).
- Position is saved as "chapter + page number". [inference] After changing font size or layout, the saved page number may no longer land on the same text.
- Reading orientation: portrait, landscape-left, landscape-right (README §11.3) [community].

### 10.3 Encoding [source]

- Only UTF-8 (BOM skipped; statistics over the first 8 KB, `good >= bad*2` means UTF-8) and GBK (FatFs `ff_oem2uni(...,936)`) (`text_encoding.cc:43-52,138-187,249`).
- **No Shift_JIS/CP932**, Big5 or UTF-16.
- Code points outside the BMP become `'?'` (`text_encoding.cc:26`, `html_content.cc:155-157`).

### 10.4 Fonts [source]

- **`.ef`** (magic `EPDFONT\0`, `main/display/font/epdfont.h:14-54`):
  - 1 or 2 bpp, 12 B per range-table entry, 16 B per glyph record.
  - Glyph cache in PSRAM, warmed per page; **no fallback font** (`epdfont.cc:400,462`).
  - Stored on SD under `fonts/`; converter `tools/epdfont/fontconvert.py`.
- **fontpack** (magic `EFNT`, 32 B header, index key `(unicode<<32)|(size<<16)|bpp`, `tools/fontpack/README.md:29-36`): mmapped from `font_data`.
  - Size allocation: UI uses 25@2, titles 30@2, body prefers `.ef` from SD and falls back to the fontpack (`book_font_runtime.cc:15-51`, `fontpack_lvgl.cc:319`).
  - `tools/fontpack/` only contains the README; the `build_fontpack.py` etc. it mentions are not in the repo.
- The device can convert a TTF into a font on the fly, but the character set can only copy an existing template `.ef` (`ttf_convert.h`). It also depends on freetype 2.14.3 (§15).
- **Japanese coverage of the bundled fonts** (from a read-only script parsing the `.ef` range tables):

| | `misans_25_2.ef` | `xiawuzhenkai_25_2.ef` |
|---|---|---|
| Total code points | 6009 | 4935 |
| JIS Level 1 kanji | 2389 / 2965 | 1782 / 2965 |
| JIS Level 2 kanji | 1178 / 3390 | 862 / 3390 |
| Hiragana / katakana | 69/86 · 77/91 | 69/86 · 77/91 |
| Missing in both | small kana ぁぃぅぇぉ, ぱぴぷぺぽ, ヌ, ヲ, ・, 〜 | Same; also missing Japanese-specific forms such as 門/駅/様 |

Conclusion: the bundled fonts **cannot typeset Japanese**; the character set was chosen from common Simplified Chinese characters.

### 10.5 Cloud progress [source]

At boot, each book's `{name, progress%, readingSeconds, todayReadingSeconds}` is POSTed to `library/sync`: up to 500 books, deduplicated by title, upload only, never pulled down (`book_progress_sync.cc:120-131,173-188`).

---

## 11. Study / dictionary / AI

- **Dictionary, word cards, vocabulary list, per-sentence audio playback, shadowing: none exist** (grep in `main/` and `tools/` for dict/vocab/flashcard/词典/单词/生词/anki finds nothing). [source]
- The only "study"-related feature is Baiwen AI: cloud ASR→LLM→TTS, plus A2UI paginated display and LaTeX math, with voice-created to-dos (README §2.1/§11.2) [community]. All of it depends on the blanked private cloud.
- MCP tools (`mcp_server.cc:44-290`) [source]:
  - Public tools: `self.get_device_status`, `self.audio_speaker.set_volume`, `self.screen.set_theme`, etc.
  - User-only tools: `self.reboot`, `self.upgrade_firmware` (**accepts any URL**), `self.screen.snapshot`, `self.assets.set_download_url`.

---

## 12. OTA and upgrades

[source]
- **Version check**:
  - URL is fixed to `cloudzao_ota_url`, an empty string in the open-source tree (`ota.cc:50-53`, `cloudzao_endpoints.c:6`).
  - Request body is xiaozhi's OTA JSON (`ota.cc:104-109`, `board.cc:104-167`), containing MAC, uuid, chip info, ELF sha256, the whole partition table and Wi-Fi info.
  - Request headers include `Activation-Version`, `Client-Id`, `Serial-Number`, `Device-Id` (`ota.cc:55-68`).
- **Response**: `activation`, `mqtt`, `websocket`, `server_time`, `firmware{version,url}`; `firmware.force` is ignored (`ota.cc:137-259`).
- **Activation**: HMAC-SHA256 of the challenge with eFuse HMAC_KEY0, POSTed to `<ota_url>/activate` (`ota.cc` around lines 603-660). [inference] Factory devices may have an HMAC key burned in eFuse (to-verify, **read-only**).
- **Verification**: only the image-format check in `esp_ota_end`, **no signature check**; secure boot is off (`ota.cc:375-383`, `sdkconfig:493`).
- **Rollback is ineffective**: `MarkCurrentVersionValid()` is called right at boot, before networking and before any self-test (`application.cc:1244-1245`).
- Skipped versions are recorded in NVS `ota/skip_ver` (`ota_ignore.cc:10-29`).
- SD upgrade see §5.2; factory full images are merged with `merge_firmware.sh` (README §14.3) [community].
- Inference: as long as the bootloader and partition table are unchanged, a bunko app image can be installed via the official "put `Metalio-E-Ink4.bin` on SD" path, or written to ota_0 / ota_1 with esptool.

---

## 13. Cloud and telemetry

- The 24 strings in `main/cloudzao_endpoints.c:4-27` are **all empty**:
  - hosts: cloudzao_host, log_upload_host, ota_url
  - third-party chat clouds: xiaozhi/yunxin url/app_key/token
  - book_tools_url
  - paths: asr, checklist ×3, weather, tts settings, push_resources, library_sync, device_location_wifi, vision_upload, device_logs

  `scripts/blank_cloudzao_endpoints.py` is a commit gate that rejects any non-empty value (`:12-40`); with an empty host `api::Url()` returns empty and boot-time sync is skipped (`api_endpoints.h:92-97`, `application.cc:1299-1306`). [source]
- README only reveals the host has the form `xxx.xxxx.cn` [community].
- **Data uploaded once endpoints are filled in** ([source]):
  - Every request carries `Device-Id` = MAC (`api_http.h:13-25`).
  - Wi-Fi BSSID list (§6).
  - Reading progress (§10.5).
  - SD logs and coredump: multipart upload to `device_logs?mac_id=<MAC>`, triggered manually from Settings (`settings_log_upload.cc:38-40,495,518`).
  - Camera JPEGs, weather requests, TTS preferences.
- Chat: Baiwen (default), Xiaozhi, Yunxin, stored in NVS `chat_cloud/provider`. WebSocket uses Bearer; MQTT+UDP uses AES-CTR (`chat_cloud.h:8-79`, `websocket_protocol.cc:128-137`, `mqtt_protocol.cc:172-183`).
- `esp_debug_cloud` is a UDP log forwarder (magic `EDC1` + MAC, default 192.168.5.67:19527), disabled at compile time (`esp_debug_cloud.h:7-12`). [source]
- [inference] Factory v2.0.65 has the endpoints filled in, so factory units upload the MAC, nearby Wi-Fi BSSIDs and reading records.

---

## 14. Value for bunko

### 14.1 Hardware fact baseline (can be copied or ported; MIT, keep the copyright notice)

| Item | What to take | Location |
|---|---|---|
| Full pin table + TCA9555 bitmap + boot levels | Use directly as the first bring-up table | `config.h`, `IOExpander.hpp:46-57`, `metalio_io_expander.cc:35-46` |
| I²C recovery technique | 9-clock recovery + expander retries | `metalio_i2c_bus.cc`, `IOExpander.hpp:316-336` |
| SSD1677 init sequence + three LUTs + LUT byte-splitting + 0x22 values | Starting point for the display driver; the 4-gray LUT comes from the FreeInk/Cross lineage | `esp_lcd_panel_ssd1677.c`, `LUT/*.h` |
| Dual-plane partial semantics (`0x26`=old, `0x24`=new), full refresh every 15 partials | Use as baseline; the ghosting threshold must be measured ourselves | `lv_adapter_display.cc:848-926` |
| 4-gray-over-B/W anti-aliasing flow | Worth borrowing; whether it pays off for small Japanese text needs measurement | `lv_adapter_display.cc:929-998` |
| Touch: CST816S address, reset via expander, sleep command, exact virtual-key coordinates | Copy | `metalio_touch.c`, `config.h:64-70` |
| Bluetooth module AT sequence + I²S slave parameters + re-send mode 1 400 ms after rail drop | **Required for any sound**, copy | `bluetooth_uart.cc:190-268`, `bt_audio_codec.cc:22-124`, `power_hw.cc:33,57-62` |
| SDMMC 1-bit + DAT3 pull-up | Copy | `SdCardManager.hpp:226-235`, `metalio_board_periph.cc:58-65` |
| TinyUSB MSC SD export + PHY switching between USJ/OTG | Reusable for the "copy books from a computer" path | `usb_virtual_disk.cc` |
| CX25601N register writes, BQ27220 register reads | Write technique can be borrowed; **parameters and policy must not be copied** | `cx25601n.cc`, `bq27220_gauge.cc` |
| `.ef` / `.ebook` formats | Reference only; Japanese typesetting needs (ruby, kinsoku, vertical) require a redefinition | `epdfont.h`, `tools/ebook/SPEC.md` |

### 14.2 Pitfalls (do not copy)

| # | Pitfall | Consequence | Source |
|---|---|---|---|
| 1 | `PwrOffTask` loops forever emitting a 5 Hz square wave; `shutdown_requested` never cleared; the 30 min standby auto power-off ignores USB | Prime suspect for issue #4-style "zombie" state [inference] | §4.4/§4.8 |
| 2 | EPD BUSY wait without timeout + Task WDT does not panic | If the panel hangs, the whole device silently freezes | `ssd1677.c:143-153`, `sdkconfig:1516` |
| 3 | VREG 4400 mV + termination disabled every 2–10 s + charger IC watchdog off | [inference] Settings persist after the ESP stops; whether 4.4 V is within the cell rating is unknown; long-term float charging is a risk | `cx25601n.cc:229,357,368` |
| 4 | ICHG default 1000 mA, IINDPM equal to ICHG | Disagrees with README's 500 mA; cell capacity unknown | `metalio_board_periph.cc:111` |
| 5 | Battery level from instantaneous voltage only, first reading taken at boot | Large jumps in readings (second symptom of issue #4) | `bq27220_gauge.cc` |
| 6 | No under-voltage check during standby; ticks do not advance in light sleep | Deep discharge [inference] | `battery_uv_shutdown.cc:71-73` |
| 7 | VBUS judged by VBUS_STAT, but D+/D− detection is off, so it reads 0 even when plugged in | BATFET may be wrongly turned off while plugged in [inference] | `cx25601n.cc:215,353` |
| 8 | Panel deep sleep never called; display socket and main rail stay powered at power-off | Panel high voltage not removed for long periods [inference] | `power_hw.cc:348-374` |
| 9 | I²S read/write with `portMAX_DELAY` + `ESP_ERROR_CHECK` | Hangs if the module gives no clock; aborts on error | `bt_audio_codec.cc:100,110` |
| 10 | `ESP_ERROR_CHECK` on the expander and `resources` at boot | A single I²C glitch causes a reboot loop | `metalio_io_expander.cc:35-46`, `lv_adapter_display.cc:1533` |
| 11 | Whole NVS erased on error | All settings and credentials lost | `main.cc:22-27` |
| 12 | Rollback confirmed at the very start of boot; OTA unsigned; MCP accepts any URL | Bad firmware cannot roll back automatically | §12 |
| 13 | Virtual keys require exact coordinate equality | Breaks if the touch chip configuration changes | `lv_adapter_display.cc:221` |
| 14 | Reader: no kinsoku/ruby/vertical, no Shift_JIS, fonts missing kana | Japanese reading unusable | §10 |
| 15 | Partial LUT differs from its source draft; FULL_FAST is dead code and enabling it would invalidate the LUT cache | Before changing waveforms, first establish which table is actually in effect | §2.3 |
| 16 | Only POWER key and timer are wake sources; BOOT, touch, RTC and accelerometer cannot wake | Another source of "pressed it and nothing happened" | `power_hw.cc:405-408` |

---

## 15. Third-party components and licenses [source]

| Component | Version | License |
|---|---|---|
| lvgl | ==9.5.0 | MIT |
| esp_lvgl_adapter / esp_lvgl_port | 0.6.3 / 2.6.3 | Apache-2.0 |
| esp-wifi-connect (vendored) | 2.6.2 | MIT |
| esp-ml307 (vendored) | 3.5.3 | yml says MIT, but the LICENSE file is Apache-2.0 |
| uart_eth_modem | 0.6.1 (with local modifications) | Apache-2.0 |
| esp-sr | ~2.2.0 | Espressif license (ESP chips only) |
| freetype | 2.14.3~1 | FTL / GPLv2 dual license |
| esp-opus-encoder, esp_new_jpeg, esp_tinyusb, usb_host_uvc, esp_video, xiaozhi-fonts, zlib | see `main/idf_component.yml` | Their own licenses |
| Fonts MiSans, LXGW ZhenKai (霞鹜臻楷), Latin Modern Math, Noto Emoji | `tools/ttf-fonts/` | Each font's own license (MiSans has commercial-use terms, to-verify) |

There is no `dependencies.lock`, so builds are not reproducible.

---

## 16. To verify

1. **Differences between v2.0.65 and v2.0.63**: no source for the factory version; power-off, charging and fuel-gauge logic may have changed. Only reverse engineering a full-flash dump or asking the vendor can settle this.
2. **Actual partition table**: whether the table on the device is `v1/16m.csv` (release flow is questionable, §1.4). Reading 0x8000 from a full-flash dump confirms it.
3. **Power-management chip model**, and its response to the P1.3 pulse: pulse width, single vs continuous, whether it can cut power with USB present. This determines how bunko powers off, and the root cause of issue #4.
4. **Cell rating**: capacity, and whether full-charge voltage can reach 4.4 V. This determines VREG and ICHG, and how to configure the BQ27220.
5. **Bluetooth audio module model**, its full AT command set (volume/EQ/power-saving commands?), power-up timing, and whether the I²S clock is always on in mode 1.
6. **Physical location of the microphone**: on the module side, or an I²S mic on the ESP side.
7. **Whether the RTC alarm can wake the power IC** (i.e. timed power-on after power-off, issue #3); where the PCF8563 INT is connected.
8. **Relationship between GDEM0397T81 and the repo datasheet DEPG0397BBS77AF11HP**; actual refresh durations with OTP waveforms vs custom LUTs (full/partial/4-gray), and BUSY polarity (code says high = busy).
9. **CST816S cover keys**: whether the cover reports any points besides these three, and the reporting behaviour during long press.
10. **eFuse state** (read-only): whether HMAC_KEY0 is burned, whether secure boot / flash encryption is enabled. Determines whether bunko can flash an app directly.
11. **Whether the 4G module and camera are actually populated** (different SKUs may exist).
12. **Whether the SD card in the issue #4 unit has `logs/metalio_coredump.elf` and `metalio_logs.txt`**: to tell whether that event was a panic, a hang or a real power-off.
13. **SD card**: actual SDMMC clock ([inference] 20 MHz), and whether cards larger than 32 GB, formatted exFAT out of the box, work (firmware does not enable exFAT).
