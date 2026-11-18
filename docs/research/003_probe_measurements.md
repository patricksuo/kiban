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
- Opening the USB-Serial/JTAG port with DTR/RTS asserted can reset the chip into download mode; consoles must set
  DTR/RTS low before opening the port.

## USB enumeration

- Enumeration on the Mac was intermittent during the session with the stock firmware running (minutes without a
  device, then present again after a power cycle). Cable orientation was suspected earlier but not confirmed
  `[to-verify]`.
