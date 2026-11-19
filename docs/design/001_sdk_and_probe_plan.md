# 001 · kiban SDK layout and the hardware probe firmware

Status: **draft for review** (2026-11-18). Nothing here has run on the device yet.

## 1. Goals

1. A pure-C ESP-IDF board SDK that both product firmwares (bunko, manabu) consume via `idf_component.yml`.
2. A **probe firmware** (`examples/probe`) to exercise every peripheral on the real device from a serial console,
   so that each driver's behaviour is `[measured]` before products depend on it.
3. Core interfaces that are small, explicit and safe by default (no hidden power actions, every wait bounded).

Non-goals for this stage: UI framework, fonts, file formats, OTA, Wi-Fi provisioning, product power policy tuning.

## 2. Repository layout

```
kiban/
  components/kiban/            # one ESP-IDF component; products depend on this only
    include/kiban/
      board.h                  # bring-up order, pin map, shared I2C bus, rails, USB mux
      ioexp.h                  # TCA9555
      epd.h                    # SSD1677 panel (full / partial / gray, LUT upload)
      touch.h                  # CST816S + cover virtual keys
      keys.h                   # POWER (GPIO3), BOOT (GPIO0), VOL± (expander)
      battery.h                # BQ27220 read path + CX25601N status (read-only)
      rtc.h                    # PCF8563
      imu.h                    # SC7A20H
      haptic.h                 # linear motor (GPIO44)
      sdcard.h                 # SDMMC 1-bit mount/unmount, rail-aware
      audio.h                  # BT audio module: UART AT + I2S slave stream
      power.h                  # sleep, wake sources, power-off (staged; see §5)
    src/ ...
    Kconfig                    # feature toggles, timeouts
    idf_component.yml          # component manifest (version, idf >= 6.0)
  examples/probe/              # the probe firmware (ESP-IDF app, console REPL)
  docs/research/  docs/design/
```

One component rather than many: products get one dependency and one version number; unused drivers cost
nothing because nothing initialises implicitly.

Toolchain: ESP-IDF **v6.0.2** `[inference]` (newest stable installed locally). The stock bootloader is IDF 5.5.x;
see §4 for why we keep it.

## 3. Interface principles

- Plain C, `esp_err_t` returns, no global constructors, no hidden tasks. Every driver has explicit
  `*_init()` / `*_deinit()` and takes its dependencies (e.g. the I2C bus handle) from `board.h`.
- **Every hardware wait has a timeout** (EPD BUSY, I2S read/write, UART AT replies). The vendor firmware waits
  forever in several places (kiban 001 §5, §7).
- **Safe defaults**: amplifier off, USB mux on the debug path, charger untouched, no power-off pulses unless
  explicitly called.
- Expander writes set the output level **before** switching the pin to output (avoid glitches on rails).
- CST816S may NACK while not touched; init must not depend on reading its ID.
- Logging via `ESP_LOG*` with a `kiban.<module>` tag; no printf in drivers.

Sketch of the shape (illustrative, not final):

```c
// board.h
esp_err_t kiban_board_init(const kiban_board_cfg_t *cfg);   // I2C bus, expander, USB mux high, rails per cfg
i2c_master_bus_handle_t kiban_board_i2c(void);
esp_err_t kiban_board_rail_set(kiban_rail_t rail, bool on);  // KIBAN_RAIL_MAIN (P0.6), KIBAN_RAIL_PANEL_SD (P0.5)

// epd.h
esp_err_t kiban_epd_init(void);
esp_err_t kiban_epd_show(const uint8_t *fb_1bpp, kiban_epd_mode_t mode, uint32_t timeout_ms); // FULL / FAST / PARTIAL
esp_err_t kiban_epd_show_gray(const uint8_t *fb_2bpp, uint32_t timeout_ms);
esp_err_t kiban_epd_sleep(void);

// audio.h
esp_err_t kiban_audio_open(const kiban_audio_cfg_t *cfg);     // module AT handshake, I2S slave, PA stays off
esp_err_t kiban_audio_write(const int16_t *pcm, size_t frames, uint32_t timeout_ms);
uint64_t  kiban_audio_frames_played(void);                    // position counter for sentence-accurate playback
esp_err_t kiban_audio_amp(bool on);
```

The final headers are written in stage order (§6) and reviewed per stage, not all up front.

## 4. Flashing strategy

- **Keep the stock bootloader and partition table.** The probe app is written only to `ota_0` @ `0x80000`
  (5 MiB) with `esptool write-flash 0x80000 probe.bin`. Never `idf.py flash` (it rewrites bootloader + table).
  `otadata` already selects `ota_0`, so no otadata write is needed `[inference]`.
- The probe does **not** initialise NVS or Wi-Fi, so the stock `nvs` partition (Wi-Fi credentials, device
  identity) stays untouched.
- Recovery: a full 16 MB flash backup of the stock device is kept offline; restoring it is one esptool command.
- IDF 6.0 app under an IDF 5.5 bootloader on ESP32-S3: expected to work `[inference]`, to be confirmed by the
  first probe boot.

### 4.1 The one real bricking risk: USB routing

Status: **resolved.** With the expander unconfigured (after a real power-off) the mux routes USB to the ESP32-S3, and ROM
download mode (hold AI, long-press POWER) is reachable over USB (research/003 B1).

USB to the ESP32-S3 goes through a mux (FSUSB42) selected by TCA9555 P0.0 (high = ESP debug/flash path).
The stock firmware drives it high at boot. **Which way the mux defaults while the expander is still an input
(power-on, ROM download mode, or a firmware that crashes before driving P0.0) is unknown** (kiban 001, to-verify).

Evidence that it defaults to the ESP side: FreeInk's Metalio bring-up never configures P0.0 — it is left as an
expander input `[source] freeink-sdk@e637713 libs/hardware/BoardConfig/include/MetalioEInk4Board.h:93-104` —
yet CrossPoint-family firmwares built on it are flashed and updated over USB on this device `[community]`.
So the mux most likely defaults to the ESP path `[inference]`.

If it nevertheless defaulted to the camera side, a probe that crashes early would leave no USB path for esptool. Mitigation:

1. **Before any flashing**, test the default with the stock firmware untouched: hold BOOT (GPIO0), power on, and
   check whether the Mac enumerates the ROM's USB-Serial/JTAG. No write is involved.
2. The probe drives P0.0 high as its first action, before anything that can fail.
3. If the default is the camera side, we need another fallback (UART0 pads / test points) before flashing.

## 5. Safety rules in the probe

| Area | Rule |
|---|---|
| Charger CX25601N | Read-only register dump. No writes until the cell rating is confirmed. |
| Power-off (P1.3 pulses) | Not wired into any command until stage P7, and then only as an explicit command with USB state logged. |
| Amplifier (P0.4) | Off at boot; only `audio` commands turn it on, and they turn it off on every exit path. |
| EPD | BUSY wait bounded (default 5 s); a timeout powers the panel rail down and reports. |
| Rails | Panel/SD rail (P0.5) and main rail (P0.6) switched only by explicit commands or driver init. |
| eFuse | Read-only summary at most. |

## 6. Probe stages

Each stage = driver code + console commands + a short `[measured]` record in `docs/research/`.

| Stage | Covers | Console commands (indicative) | Exit criterion |
|---|---|---|---|
| P0 | Boot, USB mux high, console over USB-Serial/JTAG, chip/flash/PSRAM info | `info`, `reboot` | Probe boots under the stock bootloader; console reachable; re-flash via auto-reset works |
| P1 | I2C bus, TCA9555, rails, keys | `i2c scan`, `ioexp dump`, `rail main on/off`, `keys watch` | All six I2C addresses answer; VOL± and POWER read correctly |
| P2 | SSD1677: init, full, fast, partial, 4-gray, sleep | `epd test full/partial/gray`, `epd sleep` | Visual check by the user; BUSY timings logged |
| P3 | CST816S touch + cover keys | `touch watch` | Coordinates and the three cover keys verified |
| P4 | BQ27220, CX25601N (read-only), PCF8563, SC7A20H | `bat`, `chg dump`, `rtc get/set`, `imu watch` | Values plausible; charger state read while plugged/unplugged |
| P5 | SD card, motor | `sd ls`, `sd bench`, `motor pulse` | FAT32 mount; read speed logged; motor felt |
| P6 | BT audio module: AT handshake, I2S slave clock, tone, position counter | `bt at <cmd>`, `audio tone 440`, `audio latency` | Tone audible; module model / AT set recorded; start latency measured |
| P7 | Sleep / wake sources, power-off with and without USB | `sleep light/deep`, `poweroff` | Wake sources and power-off behaviour recorded (incl. the vendor issue #4 scenario) |

P6 and P7 decide manabu's feasibility (sentence-accurate audio, battery life) and come before any product code.

## 7. Decisions needed

1. Approve this layout and stage order.
2. Approve the USB-mux test in §4.1 (no flashing; needs the user's hands on BOOT + power).
3. Approve flashing the probe into `ota_0` once P0 builds (replaces the stock firmware until restored).

## 8. Open items

Items that are known but not yet settled. Each has a trigger: the point at which it must be resolved.

| # | Item | Why it matters | How to settle | Must be settled before |
|---|---|---|---|---|
| E1 | Which partial-refresh waveform ghosts least over many page turns (vendor partial LUT vs FreeInk `lut_fast_metalio`), and how many partials before a forced full refresh | Page-turn quality and speed in the reader | Probe command that turns N pages of real text with each LUT and a configurable full-refresh interval; user judges ghosting after 10/20/50 turns; record BUSY times | bunko's page-turn implementation |
| E2 | Whether rewriting the old-frame plane (0x26) after each partial is needed | Costs ~40 ms per page turn | Run the E1 sequence with and without the resync; compare ghosting and correctness | bunko's page-turn implementation (decide together with E1) |
| E3 | Whether panel deep sleep (`0x10=0x01`) keeps controller RAM, so a partial refresh is valid after wake | Decides whether wake-from-sleep needs a ~1 s full refresh before the first page turn | Sleep the panel, wake/re-init without a full refresh, partial-refresh a known frame, check for garbage | Power policy (stage P7) |
| B1 | ~~Default of the USB mux (P0.0) with an unconfigured TCA9555~~ **Closed:** routes USB to the ESP32-S3; ROM download mode is reachable after a real power-off (research/003 B1) | — | — | — |
| W1 | ~~HTTPS cross-signed chain~~ **Closed:** `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y` (research/003 W1); required for `kiban_net` users | — | — | — |
| W2 | lwIP TCP receive window (~5.7 KB default) limits throughput on long-RTT paths | Download speed from distant servers | Raise `CONFIG_LWIP_TCP_WND_DEFAULT` / receive mailbox (PSRAM is available) and measure | Product download implementation |
| A1 | BT audio module mode 2 (headset) gets no replies | Bluetooth headphones | Wait for the vendor (model / AT docs); or try mode 2 under the stock firmware | A product feature that needs headphones |
| B2 | Intermittent USB enumeration on the Mac (cable orientation suspected) | Development friction only | Note conditions each time it happens | — |
| S1 | Deep sleep draws ~11.6 mA with rails on (research/003) — ~3.5 days standby on ~1000 mAh | Standby life; whether "sleep" should be a real power-off with an RTC wake | Unattended on battery, overnight (run at bedtime): 1 h each of (a) deep sleep with the panel/SD rail P0.5 off, (b) additionally the BT audio module unpowered if it has its own switch, (c) real power-off with an RTC power-on after 1 h, gauge RemainingCapacity read before and after (the gauge keeps counting while off) | Product sleep policy (bunko / manabu) |
| U1 | `kiban_usb` drive mode is built but not run on the device (api §5.2) | SD export to a computer; the console disappears while it runs | Probe `usb drive start 60` with a card: (1) the host shows a drive and lists the card's files; (2) copy a file, eject, wait for the timeout: console re-enumerates, `usb drive state` shows `stop ESP_OK`, `sd ls` shows the file; (3) start again and end with a POWER press; (4) end without ejecting: card still mounts; (5) whether the PHY selection (RTC_CNTL `sw_usb_phy_sel`) survives a reset taken while in OTG, and that the boot-time stop in the probe brings the console back (no reset path is typeable while active; if the console never returns, recover through ROM download mode: hold AI, long-press POWER, flash to `0x40000`); (6) copy throughput with `CONFIG_TINYUSB_MSC_BUFSIZE` 512 | A product feature that uses drive mode |
