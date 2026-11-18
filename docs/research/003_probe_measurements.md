# 003 · Probe measurements on the device

Measured with `examples/probe` on one Metalio E-Ink 4 (ESP32-S3 QFN56 rev v0.2, Wi-Fi variant per the vendor
label is not yet checked). Each entry is `[measured]` unless tagged otherwise. Dates are 2026-11-18.

## P0 · Boot

| Item | Result |
|---|---|
| Stock bootloader (ESP-IDF 5.5.x) boots an ESP-IDF **v6.0.2** app from `ota_0` @ `0x80000` | Yes. Only `0x80000` was written; bootloader, partition table, otadata, NVS untouched |
| PSRAM | 8 MB octal, memory test OK |
| Flash | 16 MB, chip reported as "boya", bootloader runs it in DIO |
| Console | USB-Serial/JTAG works |
| Reset reason after a cold power-on | 11 (`ESP_RST_USB`) |

## P1 · I2C and expander

- All six expected I2C devices answer at boot: 0x15 CST816S (answered without the screen being touched),
  0x19 SC7A20H, 0x20 TCA9555, 0x51 PCF8563, 0x55 BQ27220, 0x6B CX25601N.
- The TCA9555 answered on the first attempt; no ~2 s start-up delay on this unit.
- After board init: output latch `0xffed`, config `0xf58c` (= vendor boot configuration), inputs show VOL±
  released (high), P1.4 (accelerometer INT) high.

## P1 · Keys

| Key | Line | Measured |
|---|---|---|
| VOL+ | TCA9555 P1.0 | active low, clean press/release |
| VOL− | TCA9555 P0.7 | active low, clean press/release |
| POWER | GPIO3 | active low; reads high without an internal pull-up (external pull-up present) |
| AI key (BOOT) | GPIO0 | **no external pull-up**: as a plain input without the internal pull-up it drifts to 0 within ~1 s. Firmware must enable the internal pull-up. This floating line is the likely cause of the "every reset enters download mode" observation below `[inference]` |

## Power and power-up behaviour

- **Power-on needs a long press of POWER (~3 s).** A short press did not turn the device on.
- **A long press "off" does not power-cycle the TCA9555.** After long-press off and power-on, the expander still
  held the vendor firmware's configuration (`config=0xf58c`, output `0xffed`) instead of its power-on default
  (`0xffff`/`0xffff`). The expander therefore sits on a supply that the power key does not cut `[inference]`;
  only removing the battery (or a full drain) would reset it.
- Consequence: the USB mux state (P0.0) survives power-off. **Its default with an unconfigured expander is still
  unknown.** The probe drives P0.0 high in `kiban_board_init()`, so from now on it no longer depends on the stock
  firmware having done so.

## Download mode

- Holding the AI key (= BOOT, GPIO0) while long-pressing POWER enters ROM download mode; esptool connected over
  USB-Serial/JTAG without resetting the chip.
- After that, every chip reset issued by esptool (`--after hard-reset`, reset reason `USB_UART_CHIP_RESET`)
  re-entered download mode (`boot:0x3`) with the key released. A long-press power-off followed by a long-press
  power-on booted the app normally. Cause not yet known `[to-verify]`.
- Flashing with `esptool --after watchdog-reset` booted the new app directly (`boot:0x2b SPI_FAST_FLASH_BOOT`);
  this is the working flash workflow.
- Opening the USB-Serial/JTAG port on macOS resets the chip into the app (a fresh boot log appears) even with
  DTR/RTS set low before `open()`; probe sessions therefore start from a fresh boot.

## USB enumeration

- Enumeration on the Mac was intermittent during the session with the stock firmware running (minutes without a
  device, then present again after a power cycle). Cable orientation was suspected earlier but not confirmed
  `[to-verify]`.

## P2 · E-paper panel

Run with `epd` probe commands; visual results reported by the user looking at the panel.

| Item | Result |
|---|---|
| Init (DC13/RST18, BUSY active high) | Works; panel responds and refreshes |
| Orientation | Frame (800×480, row 0 at gate 479) top-left appears at the **top-right** of the device held portrait with the cover keys at the bottom; frame +x points **down**. Portrait UI must rotate: portrait `(px, py)` = `(479 − y, x)` of the frame |
| `FULL` (vendor MCU waveform, `0xC7`) | transfer 81 ms, BUSY ~1017–1026 ms; clean |
| `FAST` (OTP, forced temperature, `0xD7`) | transfer 80 ms, BUSY 1019 ms — **not faster than FULL** |
| `FULL_OTP` (`0xF7`) | transfer 80 ms, BUSY 1022 ms; clean white afterwards, no ghost of the previous stripes |
| `PARTIAL` (vendor partial waveform, `0xCF`) | transfer 40 ms (+40 ms 0x26 resync), BUSY 564 ms; marker → checker left **no visible ghosting** |
| Gray overlay | base full refresh BUSY 1018 ms + gray pass BUSY 151 ms (transfers 252 + 264 ms); four bands black / dark / light / white in the expected order, boundaries clear, bands uniform |
| Sleep (`0x10=0x01`) | returns OK |

Timing resolution is one RTOS tick (10 ms). `init` itself shows BUSY 0 ms because no refresh runs during init.

## P3 · Touch (CST816S)

Run with the guided `touch_test` command (on-screen targets drawn through the measured portrait mapping).

| Item | Result |
|---|---|
| Coordinate frame | Touch native portrait (x 0..479, y 0..799) matches the display's portrait frame: no swap, no mirror |
| Tap accuracy | Targets (50,50) → (52,65); (430,750) → (439,765); (240,400) → (246,422): x +2..+9, **y consistently +15..+22** (finger lands below the aimed point) |
| Swipe | (248,195) → (244,695) along a vertical arrow; 72 samples at 10 ms polling |
| Gesture register | Always 0x00; gestures must be derived from the point track (or the controller's gesture mode enabled) |
| Cover keys | Physical layout: left = round orange key, middle and right = dark pills. Each reports a fixed coordinate while held: **left (80, 900), middle (400, 900), right (240, 900)** — middle and right are swapped relative to the vendor source comments |
| Idle reads | Reads may NACK while nothing touches the panel (handled; not counted as errors) |

With the vendor's key semantics (240 = NEXT, 400 = PREV) this means: middle pill = previous page, right pill = next page.

## P2 implementation notes

What `components/kiban/src/epd.c` and the `epd` probe command were built from.

### Sources used

- Vendor firmware, <https://github.com/CloudZao/Metalio-E-INK4> @ `d12f481` (MIT): init sequence, LUT load order,
  `0x22` values and RAM plane usage `[source] main/boards/metalio-e-ink-4/display/esp_lcd_panel_ssd1677.c:115-153,163-240,333-438,565-587`;
  pins and 10 MHz `[source] main/boards/metalio-e-ink-4/config.h:42-49`; SPI3 mode 0, reset then 100 ms
  `[source] main/boards/metalio-e-ink-4/display/metalio_epd.cc:21-79`; waveforms `[source] main/display/LUT/epd_lut_fastupdate.h:12-31,
  epd_lut_partial.h:12-31, epd_lut_4gray.h:11-30`; gray overlay flow and plane encoding
  `[source] main/display/lv_adapter_display.cc:848-998, main/display/epd_gray_aa.cc:92-119`.
- FreeInk SDK, <https://github.com/Free-Ink/freeink-sdk> @ `e637713` (MIT): BUSY active high and DC13/RST18
  `[source] docs/metalio-eink4-support.md:53,62-69`; per-mode border (`0x01` full, `0x80` partial), resync of the
  old-frame plane after a refresh, OTP full refresh `0x22=0xF7`, power-down before deep sleep
  `[source] libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp:127-133,503-526,750-759,962-972`.
- The 0x1abin fork's combined text AA (<https://github.com/0x1abin/freeink-sdk> @ `2f51609`, `docs/ssd1677-text-aa.md`)
  was read for context only; nothing from it is ported.

### What the driver does

| Mode | Planes before activation | LUT | `0x22` | Source |
|---|---|---|---|---|
| `FULL` | 0x26 = 0x24 = frame | vendor `kEpdLutFastUpdate` (VCOM 0x50) | `0xC7`, then `0x3F=0x01` | vendor normal full |
| `FAST` | 0x26 = 0x24 = frame | OTP, temperature forced `0x1A=0x6A` | `0xD7`, then `0x3F=0x01` | vendor `FULL_FAST` (dead code there) |
| `PARTIAL` | 0x24 = frame (0x26 = previous) | vendor `kEpdLutPartial` (VCOM 0x00) | `0xCF`; afterwards 0x26 = frame | vendor partial + FreeInk resync |
| `FULL_OTP` | 0x26 = 0x24 = frame | OTP, internal sensor | `0xF7` | FreeInk Metalio full |
| gray | base pass as `FULL` (non-white → black); then 0x24 = dark, 0x26 = dark or light | vendor `kEpdLut4Gray` (VCOM 0x30) | `0xCC`, `0x21=0x00`; then both planes = base | vendor reading-AA overlay |

Border is written per mode (`0x01` full / gray base, `0x80` partial) `[source]` FreeInk; the vendor writes `0x80`
before partials and never restores `0x01` `[source] esp_lcd_panel_ssd1677.c:565-575`. Every BUSY wait is bounded
(default 5 s, `CONFIG_KIBAN_EPD_BUSY_TIMEOUT_MS`); the vendor loop has none `[source] esp_lcd_panel_ssd1677.c:145-153`.
BUSY timing resolution is one RTOS tick (10 ms at `CONFIG_FREERTOS_HZ=100`). The probe logs `transfer` (SPI) and
`BUSY` ms per refresh and warns when BUSY was never seen high, since a near-zero BUSY time can mean nothing was driven.

### Questions for the first device run

Answered by the P2 table above: 1 (orientation), 2 and 3 (BUSY polarity, DC/RST), 6 (gray band order).
Still open: 4, 5, 7 — tracked as E1, E2, E3 in design/001 §8.

1. **Orientation** `[inference]`. Frame row 0 is written to gate address 479 (data entry `0x11=0x01`, Y counter
   starting at 479), exactly as the vendor does. The vendor's portrait UI maps logical `(lx, ly)` to panel
   `(px, py) = (ly, 479 - lx)` `[source] main/display/display_orient.cc:36-39`, so the frame's top-left corner should
   appear at the top-right of the screen when the device is held in portrait with the cover keys at the bottom, and the
   frame reads upright with the device turned 90° counter-clockwise. `epd full marker` (square at the frame origin, an
   arrow pointing to +x along the top edge, a small dot at bottom-right) settles it.
2. **BUSY polarity** `[source]`: vendor and FreeInk both treat high as busy and the vendor enables a pull-down; the
   vendor pin spreadsheet calls the line `BUSY_N` (per FreeInk `metalio-eink4-support.md:67-69`). If `epd init`
   times out on reset with the panel rail on, polarity or DC/RST are the first suspects.
3. **DC/RST** `[source]`: DC13/RST18 follows the vendor's runnable `config.h` and FreeInk; the spreadsheet says
   DC18/RST13. A panel that never asserts BUSY after `0x20` points here before it points at waveforms.
4. **Which LUTs**: the vendor partial LUT (`0D/04` frames, VCOM `0x00`) differs from FreeInk's `lut_fast_metalio`
   (taken from a different vendor waveform document: `03/10/01/01` frames, VCOM `0x10`), and FreeInk's Metalio gray LUT
   raises VSH1 `0x41→0x46`. Only the vendor tables are ported; ghosting and gray separation are for the user's eye.
5. **Ping-pong**: the vendor sends `0x37` with byte 6 = `0x40` and writes only 0x24 for partials, relying on the
   controller to keep the old frame in 0x26 `[inference]`. kiban rewrites 0x26 after every partial (one extra ~40 ms
   transfer) so it does not depend on that; whether it is needed is untested.
6. **4-gray ramp**: the band order black / dark / light / white assumes the vendor's overlay semantics (overlay lightens
   black base pixels; MSB-only = light, both = dark) `[source] epd_gray_aa.cc:92-119, epd_i1_glyph_thin.h:33-43`. If
   the two middle bands come out swapped or indistinguishable, the encoding or VSH1 needs revisiting.
7. **Sleep**: deep sleep mode `0x10=0x01` (vendor value, never called by the vendor UI). Mode 1 is documented to keep
   RAM `[inference]`; kiban still requires a non-partial refresh after `kiban_epd_init()`.

## P4 · Gauge, charger, RTC, accelerometer

All read-only except the accelerometer configuration (CTRL_REG1/4). USB plugged in, device lying screen up.

| Item | Result |
|---|---|
| BQ27220 | 4379 mV, +6 mA, 26.0 °C, SOC 100 %, remaining/full/design **3000/3000/3000 mAh**, cycles 0, SOH 100 %, BatteryStatus 0x5268 |
| BQ27220 configuration | Design capacity 3000 mAh and ChargingVoltage 4200 mV are the chip's defaults; the cell is listed as 1000 mAh `[community]` comparisontabl.es. **The gauge is unconfigured, so its SOC / capacity figures are not meaningful** (consistent with research/001: the vendor only uses voltage and current) |
| CX25601N (programmed by the stock firmware, persists while powered) | **VREG 4400 mV**, ICHG 960 mA, IINDPM 960 mA, VBUS_STAT "none" (D+/D− detection off, see research/001), CHRG_STAT CV |
| CX25601N raw 0x00–0x3F | `ff ff 00 03 c0 0d 00 03 60 0e 20 03 c0 0f 00 0b / c0 00 30 00 03 1c a0 4f 00 c0 bd 25 3f 00 10 00 / 1d 08 a0 1c 08 00 30 04 00 … / 00 … 02 00 10 01 00 …` (0x38 part info = 0x02) |
| Cell voltage under charge | **4379 mV**, i.e. above 4.2 V. Safe only if the cell is a 4.35/4.4 V (LiHV) type. The cell rating is still unknown; the only public figure is "3.7 V nominal, 1000 mAh" `[community]`, which usually indicates a 4.2 V cell but is not authoritative |
| PCF8563 | Reads 2265-11-07 09:37:08 with the VL flag set: time not valid (never set or lost power) |
| SC7A20H | WHO_AM_I 0x11; lying flat: x ≈ −2..−15, y ≈ −17..−27, z ≈ −1000 mg (first sample −765 while settling) |

### Charger VREG change (2026-11-18)

- Goal: cap the charge termination voltage at 4200 mV until the cell rating is confirmed (user decision).
- Plain writes to 0x04 alone were ignored (read-back unchanged, I2C ACKed); adding the vendor unlock (0x70 → 0x03)
  did not help.
- **Working sequence** `[measured]`: clear EN_CHG and WDT in 0x16 (`0xa0 → 0x80`), write 0x04 then 0x05 (both, vendor
  order, even if 0x05 is unchanged), read back, then set EN_CHG again with HIZ clear (`0x16 = 0xa0`). After it:
  VREG 4200 mV, CHRG_STAT "not charging / terminated" with the cell at 4371 mV, gauge current −45 mA.
  Which of the two changes (charging disabled, or writing the high byte) is required was not isolated.
- `kiban_board_init()` enforces this on every boot; the setter refuses values above 4200 mV.

## P5 · SD card and motor

| Item | Result |
|---|---|
| SD mount | SDMMC 1-bit, DAT3 (GPIO46) pulled up; 15.2 GB SDHC card, FAT; mount ~35–40 ms; never formatted |
| Clock | 20 MHz and 40 MHz both mount |
| Read speed | 2.35 MB/s at 20 MHz, 4.51 MB/s at 40 MHz (only a 66 KB file was available; rough figures) |
| Card contents | `metalio/e-ink/logs/` holds the vendor firmware's ring log and a moved core dump (see research/001 §4.8) |
| Motor | GPIO44 on/off. A 200 ms pulse is clearly felt; the vendor's 35 ms pulse was barely noticeable to the user |

## P6 · Bluetooth audio module (first pass)

| Item | Result |
|---|---|
| UART2 115200 (TX48/RX47) | Module answers. `AT+RX=2` is answered with `AT+RX=0` (meaning unknown); `AT+MODE=1` with `SET MODE 1` ~120 ms later |
| I2S0 slave (BCLK6, WS43, DOUT7), 32-bit stereo | Module supplies the clock after `SET MODE 1`: writing 16000 frames takes ~922–928 ms (1 s minus the initial DMA fill) → 16 kHz |
| Open sequence | ~2.7 s including the vendor's 700 ms gap and a 2 s confirmation window |
| Tone 440 Hz, 1 s, amplifier P0.1=0 / P0.4=1 | Audible at 5 %, 15 %, 30 % (linear sample scale). 30 % is already loud; the user heard slight distortion at 30 %, none at 5 % and 15 % |
| Not yet measured | Start latency (write → sound), module model / AT command set, microphone path, behaviour after main-rail power cycles, current draw |

### P6 · Start latency (microphone loopback)

Method: the module's input stream (I2S0 RX, DIN17, same slave clock) records while an 80 ms 1 kHz beep is written at
5 % volume, amplifier already on and settled; onset = first 1 ms bin above 4× the noise floor, timed from the
`kiban_audio_write()` call. The microphone signal is on **slot 0 only** (slot 1 is always 0).

| TX/RX DMA ring | Onset after write call (5 runs) |
|---|---|
| 6 × 240 frames (IDF default, 90 ms) | 62.4 – 96.4 ms (first session: 55.6 – 73.5 ms) — position in the ring varies |
| 4 × 64 frames (16 ms) | 57.5, 59.6, 57.6, 57.6, 59.6 ms |
| 3 × 32 frames (6 ms) | 58.6, 57.7, 57.7, 57.7, 59.7 ms |

So the loop (module output path + speaker → microphone + module input path) adds a **fixed ~58 ms, stable to about
±1 ms**; output latency alone is at most that. With the default DMA ring the latency is larger and varies by tens of
ms. `kiban_audio_set_dma()` selects the ring; products needing accurate starts should use a small ring (e.g. 4 × 64)
and keep the ring fed (underrun behaviour with a real decoder is not yet measured) `[inference]`.

### P6 · Module identification

Read-only probing only (no `=` commands). The module answers `AT ERROR` (~120 ms) to `AT+VER`, `AT+VERSION`, `AT+VER?`,
`AT+HELP`, `AT+MODE?`, `AT+RX?`, `AT+NAME?`, `AT+ADDR?`, `AT+GMR`, and nothing to a bare `AT`. Model and firmware
version cannot be read over UART; the only known command set is the vendor's (`AT+RX=`, `AT+TX=`, `AT+MODE=`,
`AT+INQUIRING`, `AT+CONNECT=`, `AT+PP=`, `AT+BTSCO=`). Identifying the chip needs the board marking or the vendor.

## P7 · Sleep and wake (part 1, no user action)

| Item | Result |
|---|---|
| Light sleep, timer 3000 ms | Woke by timer after 2991 ms. USB-Serial/JTAG drops during light sleep (output printed right after wake is lost) |
| Deep sleep, timer 5000 ms | Woke by timer; 5664 ms measured from sleep entry to the probe's report (includes ~0.66 s of boot). TCA9555 state retained across deep sleep |
| PCF8563 countdown interrupt (TIE, 5 s) | No lasting change on GPIO1 (touch INT), GPIO2 (expander INT), GPIO3 (POWER) or any TCA9555 input when the timer expired. The RTC INT line is not routed to these; whether it reaches the power-switch chip (power-on alarm) is open `[to-verify]` |
| TCA9555 P1.2 (unmapped in vendor code) | Toggles irregularly (bursts of ~20–100 ms, roughly every second) independent of the RTC timer; each change pulses the expander INT (GPIO2). Source unknown `[to-verify]` |

## P7 · Wake by POWER, hardware power-off, RTC power-on

| Item | Result |
|---|---|
| Light sleep, POWER key wake (GPIO3 low level) | Woke on a short press after 14.5 s; cause GPIO |
| Deep sleep, POWER key wake (EXT1 any-low on GPIO3) | Woke on a short press after 17.2 s; cause EXT1 |
| Hardware power-off (`kiban_power_off`: P1.3 toggled every 100 ms) **with USB attached** | Power was cut: USB disappeared within ~2 s of the first pulse (20:33:21 → 20:33:23); the next boot was a power-on reset. This contradicts the FreeInk note that USB keeps the board powered `[measured]`, this unit, twice |
| Power-on after hardware power-off | A short POWER press does nothing; a ~3 s press boots |
| **RTC power-on** | PCF8563 countdown armed for 60 s, then hardware power-off with USB attached: the board powered itself on 59–60 s later (USB re-enumerated at 20:34:20, power-on reset, no key pressed). **The RTC INT line reaches the power-switch chip: scheduled wake from true power-off works** |

Consequences `[inference]`:
- A real "off" with a scheduled wake is available (RTC alarm/timer + power-off), instead of keeping the board in deep sleep
  with all rails powered.
- The vendor issue #4 log (pulses started, firmware still alive ~2 s later) is not explained by "USB prevents power-off"
  alone on this unit; the zombie state is still best explained by the vendor's unbounded `PwrOffTask` and its
  side effects, but the exact power-switch behaviour in that state is open.
