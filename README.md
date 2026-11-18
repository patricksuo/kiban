# kiban (基盤)

Board SDK for the **Metalio E-Ink 4** (ESP32-S3, SSD1677 800×480 e-paper, CST816S touch,
Bluetooth audio module). Pure C, packaged as ESP-IDF components. Product firmwares consume it
as a pinned git dependency in `idf_component.yml`.

Scope:
- Drivers: SSD1677 panel, CST816S touch, TCA9555 IO expander, BQ27220 fuel gauge,
  CX25601N charger, PCF8563 RTC, SDMMC, linear motor, SC7A20H accelerometer,
  Bluetooth audio module (UART AT + I²S slave).
- Power policy: power-off, sleep and wake, behaviour with USB attached, charging safety —
  written once, here.
- Hardware facts: pin map, power-up order, known pitfalls (`docs/research/`).

Out of scope: layout, fonts, UI, content formats, playback logic — those belong to products.

Status: early. Hardware research: `docs/research/001` (vendor firmware), `002` (FreeInk SDK and stack choice).

Board logic ported from the vendor firmware ([CloudZao/Metalio-E-INK4](https://github.com/CloudZao/Metalio-E-INK4))
and [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk), both MIT; attribution is kept in the ported files.

License: MIT.
