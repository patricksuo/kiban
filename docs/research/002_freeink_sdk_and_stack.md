# 002 FreeInk SDK evaluation and choosing one of three stacks

- Surveyed: 2026-11-12
- Subjects:
  - FreeInk SDK <https://github.com/Free-Ink/freeink-sdk> @ `e637713a533ea7583ba73fa2052967c21cf0335d` (2026-11-11)
  - Fork <https://github.com/0x1abin/freeink-sdk> @ `2f51609b083a7354624989469689cab4d3a6162d` (2026-11-06)
  - Official firmware <https://github.com/CloudZao/Metalio-E-INK4> (MIT) @ `d12f481` (the repo's only commit, see 001); paths cited below as `official:<repo-relative path>`
- Method: read source and repository docs only. No third-party code was built or executed; no device was touched.
- Evidence levels: `[source]` = code/docs read, with `file:line` (FreeInk paths relative to repo root); `[community]` = claims in READMEs/docs/commit messages; `[inference]` = our judgement; `[to-verify]` = needs a real device.

---

## 0. Conclusions first

| Question | Answer |
|---|---|
| What FreeInk is | A **C++20** library collection on Arduino-ESP32 (PlatformIO, arduino core 3.3.x / ESP-IDF 5.5.x), with drivers selected by board profile `[source] platformio.sample.ini:14-21` |
| Maturity of Metalio support | Added from 2026-09-15, about 15 commits; repo docs still say "compiled, host-tested; **on-device validation pending**" `[source] docs/metalio-eink4-support.md:6,173-177`; but commits contain on-device evidence: power-off pulse, keys/touch/fuel gauge/IMU/RTC tested on the Wi-Fi variant `[community] 4713b9e, c30fc13` |
| Usable on Metalio under pure ESP-IDF? | **Not directly, today**: the ESP-IDF component only includes display/UI/font, `Wire` is declared but not implemented, and the Metalio display path must power up through the TCA9555 (I²C) `[source] docs/esp-idf.md:70-76, idf/include/Wire.h:16-26, libs/display/FreeInkDisplay/src/bus/EpdBus.cpp:30-33` |
| Does audio meet "sentence-accurate playback start"? | No: accepts only 16 kHz WAV, creates a new task on every `play()`, toggles the amplifier per sentence, no playback-position API `[source] AudioManager.cpp:337-339,360,443-444` |
| Recommendation `[inference]` | **Option 3: ESP-IDF + C, porting the board logic of FreeInk / the official firmware to C file by file** (keeping MIT attribution). If an EPUB engine is wanted, evaluate separately wrapping FreeInkBook (standalone C++17, no Arduino dependency) behind `extern "C"`, but it supports neither ruby nor vertical text |

---

## 1. Architecture

### 1.1 Framework and build

| Item | Fact | Evidence |
|---|---|---|
| Main build | PlatformIO + `framework = arduino`, pioarduino platform 55.03.37 (arduino core 3.3.x / IDF 5.5.x) | `[source] platformio.sample.ini:14-17` |
| Language standard | `-std=gnu++2a`, gnu++11 removed | `[source] platformio.sample.ini:20-21,28-29` |
| Metalio environment | `esp32-s3-devkitc1-n16r8`, qio_opi, `-DFREEINK_DEVICE_METALIO_EINK4=1`, lib_deps pull Haptic/Rtc/Power/Audio/Mic/Imu via symlink | `[source] platformio.sample.ini:405-424` |
| ESP-IDF component | Root `CMakeLists.txt` registers only FreeInkDisplay + BoardConfig + FreeInkUI + FreeInkFont(stb) + the `idf/` Arduino shim | `[source] CMakeLists.txt:13-34` |
| Shim scope | `Arduino.h/SPI.h/Wire.h`; SPI actually implemented (SPI2_HOST, 4 KB DMA bounce buffer); **Wire declared only, not implemented**; input/battery/SD/audio managers "still Arduino-only" | `[source] docs/esp-idf.md:58-76`; no `Wire` in `idf/src/ArduinoCompat.cpp` |
| Boards validated under ESP-IDF | Only Seeed reTerminal Sticky, IDF 5.5.2 | `[source] docs/esp-idf.md:78-81` |
| Device selection | Compile-time macro `FREEINK_DEVICE_*`, one `constexpr BoardProfile` table (BoardConfig.h, 2487 lines) | `[source] libs/hardware/BoardConfig/include/BoardConfig.h:93-107,1764-1784` |

### 1.2 Layering

```
app ─▶ EInkDisplay(=FreeInkDisplay facade) ─▶ PanelDriver(virtual base) ─▶ Ssd1677Driver … ─▶ EpdBus(SPI/GPIO)
    ─▶ InputManager / BatteryMonitor / SDCardManager / AudioManager / Microphone / Rtc / Imu / HapticManager / PowerManager
           └─ read BoardConfig::ACTIVE (profile); Metalio-specific board logic in header-only MetalioEInk4Board.h / MetalioAudio.h
```
`[source] README.md "Architecture"; BoardConfig.h:2343-2345,2414-2419`

### 1.3 C++ usage (per library, excluding test/)

| Library | Lines | `virtual` | `std::function` | `template<` | Arduino `String` |
|---|---:|---:|---:|---:|---:|
| FreeInkDisplay | 13,007 | 52 | 0 | 2 | 0 |
| InputManager | 5,065 | 0 | 0 | 0 | 0 |
| SDCardManager | 989 | 0 | 0 | 0 | 15 |
| AudioManager | 691 | 0 | 2 | 0 | 0 |
| BatteryMonitor | 1,333 | 0 | 0 | 0 | 0 |
| SecureNet | 1,370 | 0 | 8 | 0 | 0 |
| FreeInkUI | 14,240 | 21 | 0 | 68 | 5 |
| FreeInkBook | 59,836 | 23 | 0 | 0 | 0 |

`[source]` grep counts. Key points:

- Board helpers use `std::mutex`, function-local `static` state, and `Wire`/`HardwareSerial` global objects `[source] MetalioEInk4Board.h:5-8,46-54,82-87; MetalioAudio.h:21-27`
- Audio sources are `std::function` callbacks `[source] AudioManager.h:32-38`; the SD interface returns `String`, `std::vector<String>`, SdFat `FsFile` `[source] SDCardManager.h:18-21,43-77`
- Classes are mostly singletons or global instances (global `EInkDisplay display(...)`, `HapticManager::getInstance()`) `[source] docs/esp-idf.md:41; docs/metalio-eink4-support.md:102`

### 1.4 Threading model

| Task | Where | Evidence |
|---|---|---|
| Main loop `setup()/loop()` (Arduino loop on core 1) | app | `[source] AudioManager.cpp:357-358` comment |
| `audio_play`: created on every `play()`, 8 KB stack, priority 10, core 0, deletes itself when done | AudioManager | `[source] AudioManager.cpp:360,480` |
| `fi_input`: optional async key/touch polling (15 ms) | InputManager | `[source] InputManager.cpp:322; InputManager.h:265-272` |
| `ble-conn` | BleKeyboardHost | `[source] BleKeyboardHost.cpp:684` |
| I²C mutex: on Metalio a recursive FreeRTOS lock is enabled for the main loop (input) and render task (fuel gauge) competing for the bus | I2cBusLock | `[source] I2cBusLock.h:1-45`; `[community] c30fc13` (without the lock the Wi-Fi variant lost most key presses) |

Display refresh has three APIs: sync / async / deferred (`displayBuffer` / `displayBufferAsync` / `triggerDisplay`+`completeDisplay`) `[source] FreeInkDisplay.h:167-246`.

### 1.5 Memory

- Framebuffer sized to the actual panel (800×480/8 = 48,000 B); with `FREEINK_FB_PSRAM` it goes into PSRAM, falling back to the internal heap on failure `[source] FreeInkDisplay.cpp:443-455`
- Single buffer by default (controller RAM keeps the previous frame) `[source] platformio.sample.ini:24-26`
- FreeInkBook uses only a caller-supplied arena: no malloc/free, no DOM `[source] docs/freeink-book.md:16-27`

---

## 2. Coverage of Metalio subsystems

Status: **impl** = implemented; **dev** = on-device evidence; **stub/none** = absent.

| Subsystem | FreeInk upstream | Status | Notes / evidence |
|---|---|---|---|
| Display SSD1677 800×480 | Full (OTP waveform 0xF7) / fast and window (external LUT `lut_fast_metalio`, 0xCC) / half (two fast refreshes via black) / deferred refresh | impl; panel image quality not yet verified on device | `[source] metalio-eink4-support.md:123-151; Ssd1677Driver.cpp:118-131; Ssd1677Luts.h:172-175` |
| Grayscale | 4-level gray anti-aliasing LUT (X4 waveform + VSH1 0x41→0x46) | impl | `[source] Ssd1677Luts.h:89-153`; `[community] 9c00674` |
| Pins | SCLK14 MOSI8 CS45 **DC13 RST18** BUSY9 (active high) 10 MHz; the official table has DC/RST swapped, FreeInk follows the official example code | impl; **DC/RST needs on-device confirmation** | `[source] metalio-eink4-support.md:53,62-65`; `official:main/boards/metalio-e-ink-4/config.h` likewise DC13/RST18, SPI3_HOST |
| Touch CST816S | Tap/swipe/long press, coordinate rotation, cover virtual keys HOME/NEXT/PREV (y=900 area); NACKs until touched, so not detected via ID register | impl + dev | `[source] metalio-eink4-support.md:79-100`; `[community] 946e6ac` (switched to continuous polling during contact), `c30fc13` (touch works on Wi-Fi variant) |
| Keys | BOOT0=confirm, GPIO3=power, TCA9555 P0.7/P1.0=volume∓→down/up | impl + dev (Wi-Fi variant) | `[source] metalio-eink4-support.md:57-58`; `[community] c30fc13` |
| TCA9555 power-up sequence | Write output levels before changing direction; turn on main rail P0.6, display/card rail P0.5, release touch reset after 10 ms, wait 120 ms; on the Wi-Fi variant I²C only responds about 2 s after power-up, `ensureBooted()` has retries and 9-pulse unlock | impl + dev | `[source] MetalioEInk4Board.h:82-126,58-77`; `[community] 29ae30b` (two hardware variants: Wi-Fi / 4G) |
| Power-off | Amplifier off, main rail and display/card rail kept on, **P1.3 toggled every 100 ms until the power-switch chip cuts power**; never returns on battery, returns after `maxMs` (default 5 s) with USB present so the caller can fall back to deep sleep | impl + dev | `[source] MetalioEInk4Board.h:163-178`; `[community] 4713b9e` ("the next power-key press is a power-on reset"); consistent with `official:main/boards/common/power_policy/power_hw.cc:361-385` |
| Deep sleep | ESP deep sleep **does not cut board power** (main rail, display/card rail, 4G, Bluetooth audio module all stay on); before sleeping only the amplifier and motor are turned off, display/card rail kept; GPIO3 wake | impl; sleep current to-verify | `[source] metalio-eink4-support.md:117-121; PowerManager.cpp:114-121` |
| USB presence detection | Metalio profile has no usbDetect and no charger address ⇒ `isExternalPowerPresent()` reports "unknown"; charge direction only from the sign of the gauge current | impl (degraded) | `[source] BatteryMonitor.cpp:945-1012; BoardConfig.h:1781` |
| Fuel gauge BQ27220 @0x55 | Reads SoC / voltage / current only; `designCapacityMah=0` ⇒ design capacity **not written** | impl + dev | `[source] BoardConfig.h:594-609,1781; BatteryMonitor.cpp:799-803`; `[community] c30fc13` |
| Charger CX25601N | **No backend, writes no registers**, "keeps its existing hardware state" | none (intentional) | `[source] metalio-eink4-support.md:29-30,46-47` |
| SD | Native 1-bit SDMMC (CLK38 CMD40 D0=39), GPIO46 (DAT3) pulled up before init; SdFat on top | impl; on-device to-verify | `[source] SdmmcBlockDevice.cpp:25-38; metalio-eink4-support.md:72-77` |
| Internal filesystem | Not provided by the SDK (left to Arduino's LittleFS etc.) | none | `[source]` no such library |
| Wi-Fi | SDK does not manage connectivity; `SecureNet` is just an HTTPS client (`std::function` sink, resumable download); `NearbyTransfer` uses Arduino `WiFi` directly | partial | `[source] SecureNet/include/ResumableFetch.h:1-40; NearbyTransfer.cpp:7,139` |
| BLE | Only a BLE HID keyboard host (NimBLE), nothing audio-related | partial | `[source] BleKeyboardHost.cpp:3-25` |
| Audio output | See §2.1 | impl; **on-device to-verify** | `[source] metalio-audio.md:95-98` |
| Microphone | 16 kHz mono, left channel >>12 saturated; at most 32 samples per `read()`; can run concurrently with playback | impl; on-device to-verify | `[source] metalio-audio.md:48-55; MetalioPcm.h:14-18` |
| RTC PCF8563 @0x51 | Read/write time; **timed wake only supported on RX8130**, not available on Metalio | impl (no alarm) | `[source] Rtc.cpp:122,151,301-305`; `[community] c30fc13` |
| Motor GPIO44 | LEDC PWM intensity/pulse/pattern (HapticManager); plus a blocking `vibrate()` | impl | `[source] HapticManager.cpp:54-88; MetalioEInk4Board.h:154-161` |
| IMU SC7A20H | Probes 0x19 then 0x18, ±2 g / 100 Hz polling, interrupt (P1.4) unused | impl + dev | `[source] Imu.cpp:190,331-332; metalio-eink4-support.md:40-43` |
| 4G NT26 | Not driven (no power switch, always on; vendor-private 2 Mbaud framing protocol, no AT) | none | `[source] metalio-eink4-support.md:32-35` |
| USB | USB MSC exporting the SD card (Arduino `USBMSC`), off by default, enabled by CrossPoint | impl | `[source] UsbMassStorage/src/*.cpp:6-7,21; metalio-eink4-support.md:153-160` |
| OTA | No network OTA; only `RecoveryBoot` (key combo back to ota_0 / flash next slot from SD `update.bin`) | partial | `[source] RecoveryBoot.h:1-25` |

### 2.1 Audio (most critical for study features)

Hardware path `[source] MetalioAudio.h:44-72; metalio-audio.md:65-75`:

- ESP32-S3 is the **I²S slave**: BCLK6 / WS43 / DOUT7 / DIN17, Philips format, two 32-bit slots, 16 kHz; clock provided by the external Bluetooth audio module.
- Over UART2 (TX48/RX47, 115200) the module is sent `AT+RX=2`, then after 700 ms `AT+MODE=1`, to make it output clocks; responses are not read back. The official firmware additionally has `AT+MODE=2/3` and `AT+RX=1` (for Bluetooth routing) `[source] official:main/display/screen/bluetooth_screen/bluetooth_uart.cc:192-212`.
- TCA9555 P0.1 low = ESP32 path selected, P0.4 = amplifier enable. The module has **no power switch and no known sleep command** `[source] metalio-eink4-support.md:36-38`.

FreeInk implementation `[source] AudioManager.cpp:320-480`:

| Aspect | Implementation | Impact on "sentence-accurate playback start" `[inference]` |
|---|---|---|
| Input format | Only 16 kHz, 16-bit PCM **WAV** (header parsed), via `std::function` read/seek callbacks | PCM from the Opus decoder needs a fake WAV header or a modified library |
| Start | Every `play()`: `stop()` → parse header → create task → write 128 frames of silence → **enable amplifier via I²C read-modify-write** → only then send data | Start latency includes task creation + two I²C transactions + amplifier settling; sentence onset may be eaten by amplifier power-up |
| End | On normal end, pads 24×128 frames of silence to drain DMA, then disables the amplifier | Amplifier toggled between every sentence: risk of pops / clipped words |
| Position | No played-sample count, no timestamps, no on_sent callback | Cannot account for "when does sample N reach the speaker" |
| Volume | Software squared gain, `int16 × gain` into a 32-bit slot `[source] MetalioPcm.h:8-12` | Usable |
| DMA | Default descriptors from `I2S_CHANNEL_DEFAULT_CONFIG` (docs say 6), `auto_clear` | Buffer depth not tuned for low latency |
| Module internal latency | Unknown; the ESP only decides when samples enter I²S, whether the module has a DSP buffer inside is unknown | `[to-verify]`: measure on device the fixed offset from I²S write → speaker output |

Summary: sentence-accurate start needs "I²S TX always on sending silence + amplifier always on + sample insertion by frame count + calibration against measured module latency"; this must be written ourselves, FreeInk's AudioManager cannot be reused `[inference]`.

### 2.2 Charge voltage risk

| Source | Behaviour | Evidence |
|---|---|---|
| FreeInk upstream | Does not touch the charger | `[source] metalio-eink4-support.md:46-47` |
| Official firmware | At init disables HIZ and the charger watchdog, **VREG = 4400 mV**, ICHG = 500 mA (later changed per settings, default 1000 mA), writes 4 vendor-private registers, enables EN_CHG; when the gauge reads ≤3100 mV and no adapter is present, writes REG0x18=0x01 to cut BATFET | `[source] official:main/boards/common/power/cx25601n.cc:45-46,355-389; official:main/boards/metalio-e-ink-4/periph/metalio_board_periph.cc:96-110` |

`[inference]`:

- A 4.40 V cutoff is only safe for high-voltage cells rated 4.40 V nominal; the cell spec has not been verified by us. If it is a 4.20 V cell, the official setting overcharges.
- With FreeInk (no writes), the charger stays in "whatever was last written or the power-on default": right after switching from the official firmware it is most likely still 4400 mV; if the chip watchdog defaults to on and resets the registers, it returns to the chip defaults (values not checked, `[to-verify]`/check datasheet).
- FreeInk also has no under-voltage BATFET cut-off; deep sleep does not cut power (deep-sleep row in §2) + no under-voltage protection ⇒ long storage may deeply discharge the cell `[inference]`.
- Whichever path is chosen, **check the cell spec first, then decide VREG**, and implement under-voltage shutdown ourselves.

---

## 3. Fork `0x1abin/freeink-sdk`

| Item | Fact | Evidence |
|---|---|---|
| Relation to upstream | merge-base `1e1cec25`; 127 commits ahead of upstream, 52 behind; 115 commits by the maintainer, mostly Codex-generated PRs | `[source] git log` |
| Positioning | BSP for the CrossMux reader: official devices use the upstream profiles, Murphy M4 / Waveshare 3.97 use its own `CrossMuxSsd1677Driver`, Read Pico uses epdiy parallel | `[source] docs/crossmux-bsp-policy.md:1-45` |
| Additions | `CrossMuxSsd1677Driver` (1015 lines), `EpdiyLcd` library (includes epdiy, **COPYING is GPL/LGPL**), SSD1677 text anti-aliasing "endpoint retention" (Metalio enables reading transitions by default), large FreeInkUI changes (+1023 lines), `AudioManager::playPcm()` / `requestRestart()` | `[source] git diff --stat 1e1cec25..HEAD; docs/ssd1677-text-aa.md:4,29-39,65; AudioManager.h (fork)` |
| Metalio charger | An early standalone port had `configureCharger()` (VREG 3840–4800 mV supplied by the caller) in the file `MetalioEink4Board.h`; **it no longer exists in the HEAD tree**, which now uses upstream's `MetalioEInk4Board.h` | `[source] e1008fc; git ls-tree HEAD libs/hardware/BoardConfig/include/` |
| Metalio speaker | HEAD's AudioManager removed the Metalio branch; `present()` does not recognise `MetalioModule` ⇒ `begin()` returns false, **effectively no speaker**; the microphone still references `MetalioAudio.h` | `[source] fork AudioManager.cpp:73-77,122-150; fork Microphone.cpp` |
| Conclusion `[inference]` | No extra value for this project: the display AA tuning is useful as reference, the rest is reader UI and other boards; Metalio audio actually regressed | — |

---

## 4. Option 1: FreeInk + a C wrapper layer

### 4.1 What the facade would look like (header only)

```c
/* bunko_hal.h — extern "C", FreeInk C++ behind it */
bool  hal_board_init(uint32_t i2c_wait_ms);          /* holdPowerRails + ensureBooted */
bool  hal_power_off(uint32_t max_ms);                /* metalio::powerOff */
void  hal_deep_sleep_until_power_key(void);

bool  hal_epd_init(void);
uint8_t *hal_epd_framebuffer(void);                  /* 48,000 B, PSRAM */
void  hal_epd_show(hal_refresh_t mode);              /* FULL/HALF/FAST */
void  hal_epd_show_window(uint16_t x,uint16_t y,uint16_t w,uint16_t h);
void  hal_epd_show_gray(const uint8_t *lsb,const uint8_t *msb);
void  hal_epd_sleep(void);

bool  hal_input_init(void);
bool  hal_input_pop(hal_key_t *key);                 /* beginAsync + popPress */
bool  hal_touch_pop(float *x,float *y);

bool  hal_batt_read(uint16_t *pct,uint16_t *mv,int16_t *ma);
bool  hal_rtc_get(struct tm *t);  bool hal_rtc_set(const struct tm *t);
bool  hal_imu_read(float g[3]);
void  hal_haptic_pulse(uint8_t intensity,uint16_t ms);

/* SD: FreeInk exposes SdFat FsFile/String, the C side must rewrap everything */
bool  hal_sd_mount(void);
int   hal_sd_open(const char *path,int flags);  int hal_sd_read(int fd,void*,size_t); ...

/* Audio: can only wrap WAV playback; sentence-accurate start must be our own code, i.e. bypassing AudioManager */
bool  hal_audio_play_wav(const uint8_t *buf,size_t len);
```

### 4.2 What leaks through

| Leak | Source | Consequence `[inference]` |
|---|---|---|
| Arduino runtime | The whole SDK assumes arduino-esp32; the ESP-IDF component only covers display, and Metalio display needs `Wire` | Either use arduino-esp32 as an IDF component (IDF version locked by arduino core to 5.5.x), or write our own `TwoWire` (I²C) + `HardwareSerial` (UART) shims |
| `std::function` callbacks | AudioManager, SecureNet | C side can only pass function pointer + `void*`; the facade needs another adaptation layer |
| `String` / `std::vector<String>` / `FsFile` | SDCardManager | SD file API essentially must be rewrapped; or bypass it entirely with ESP-IDF `esp_vfs_fat_sdmmc` |
| Global objects / function-local statics | `Wire`, `HardwareSerial uart{2}` inside `audioBus()`, `EInkDisplay display` | Init order lives in C++ static constructors, invisible to C code |
| Serialisation requirements | Board calls must be on one "hardware task", not in an ISR; I²C lock only enabled on Metalio | Our task model has to accommodate the SDK's assumptions |
| `loop()` mindset | Input defaults to `update()` polling; async mode needs `beginAsync` | Acceptable, but a different rhythm from an event-driven C core |
| Compile-time profile | `FREEINK_DEVICE_*` + giant `BoardConfig.h` | Every upstream profile change must be rechecked |

### 4.3 License, activity, lock-in

- License: MIT, derived from OpenX4 community-sdk (MIT), NOTICE keeps attribution `[source] LICENSE:1-3; NOTICE:1-40`. Third-party: FreeType, expat, miniz, pngle, hyphen patterns, each with its license under `third_party/` `[source]`.
- Activity: first commit 2026-06-02, 883 commits from 55 authors in total, 613 by one person; 166 commits in the last 30 days `[source] git log`. Single-person-led, very fast iteration.
- Metalio changes: about 15 commits since 09-15, including **behaviour-level** changes such as switching refresh to an external LUT, the I²C lock, touch polling strategy and the power-off sequence `[source] git log --grep=metalio`.
- Lock-in risk `[inference]`: high. Board logic is tied to the Arduino runtime, C++ singletons and compile-time profiles; upstream moves fast, so the wrapper needs regression testing on every sync; audio and power are exactly the parts we most need to control and most need to change.

---

## 5. Option 2: ESP-IDF + C from scratch

Drivers to write and size estimates (line counts for a C implementation, `[inference]`; reference line counts are `[source]` actual):

| Driver | Estimate | Reference |
|---|---:|---|
| I²C bus + mutex + stuck-bus unlock + slow-boot retries | 150 | FreeInk `MetalioEInk4Board.h:56-126`; official `metalio_i2c_bus.cc` (60) |
| TCA9555 (power-up sequence, read-modify-write, key sampling) | 200 | official `metalio_io_expander.cc` (66) + `metalio_buttons.cc` (298) |
| SSD1677 + waveforms (full/fast/window/half, grayscale) | 900 | official `esp_lcd_panel_ssd1677.c` (627, **itself C**) + `display/LUT/*.h`; FreeInk `Ssd1677Driver.cpp` (1025) + `Ssd1677Luts.h` |
| CST816S + cover virtual keys | 250 | official `metalio_touch.c` (145, C) |
| BQ27220 read-only | 120 | official `bq27220_gauge.cc` (264) |
| CX25601N (VREG/ICHG/watchdog/under-voltage BATFET cut) | 300 | official `cx25601n.cc` (782) |
| Power: power-off pulse, deep sleep, wake sources | 200 | official `power_hw.cc` (466); FreeInk `powerOff` |
| Audio: UART AT + slave I²S full duplex + always-on silence stream + frame-counted start | 400 | official `bt_audio_codec.cc` (124); FreeInk `MetalioAudio.h` |
| PCF8563 / SC7A20H / motor | 300 | official `pcf8563.cc` (186), `sc7a20h.cc` (135), `metalio_vibe.cc` (91) |
| SD (ESP-IDF `sdmmc` + FATFS, GPIO46 pull-up) | 80 | FreeInk `SdmmcBlockDevice.cpp:25-38` |
| **Total** | **≈2,900** | |

Not in the table: Wi-Fi / HTTPS / NVS / OTA use native ESP-IDF components; USB MSC uses the TinyUSB component; BLE is not needed this phase.

Nature of the reference sources: the official firmware is a xiaozhi-lineage C++ project (IDF ≥5.4), but within the Metalio board code the panel and touch are C files and the rest is "C-style C++", so porting to C is cheap `[source] official:main/idf_component.yml:120-121`.

---

## 6. Option 3: ESP-IDF + C, porting FreeInk / official logic file by file

The only difference from option 2 is *how* it is written: instead of starting from datasheets, sequences **already debugged on real hardware** are moved to C one by one, keeping MIT attribution in file headers.

Mandatory "known pitfalls" to carry over (all sourced):

| Pitfall | Source |
|---|---|
| On the Wi-Fi variant I²C only responds about 2 s after power-up; needs retries + 9-pulse unlock | `[community] 29ae30b`; `[source] MetalioEInk4Board.h:56-112` |
| Multiple tasks competing for I²C cause `ESP_ERR_INVALID_STATE` and lost key presses | `[community] c30fc13` |
| TCA9555: write levels before changing direction, to avoid a spurious power-off pulse and amplifier pops | `[source] MetalioEInk4Board.h:113-116` |
| Power-off requires continuous toggling of P1.3 at a 100 ms period; a single pulse does not work | `[community] 4713b9e` |
| At power-off P0.5 must not be cut first (shared by display and card) | `[source] metalio-eink4-support.md:111-113` |
| CST816S NACKs when not touched, so presence cannot be detected via ID; poll continuously during contact | `[source] metalio-eink4-support.md:98-100`; `[community] 946e6ac` |
| GPIO46 (DAT3) must be pulled up first | `[source] SdmmcBlockDevice.cpp:25-38` |
| First fast refresh upgraded to a half refresh to establish a baseline; sync both RAMs after refresh | `[source] metalio-eink4-support.md:140-144` |
| DC/RST contradict each other in two official sources | `[source] metalio-eink4-support.md:62-65` |
| GPIO44 is the motor, not UART RX | `[source] metalio-eink4-support.md:68-69` |

### Comparison of the three options

| Aspect | 1 FreeInk + C wrapper | 2 ESP-IDF + C from scratch | 3 ESP-IDF + C, file-by-file port |
|---|---|---|---|
| Effort to first light `[inference]` | Low (if the Arduino runtime is accepted); medium if insisting on pure IDF (write Wire/Serial shims + managers are not in the component) | High (≈2.9k lines + rediscover pitfalls ourselves) | Medium (≈2.9k lines, but the sequences have known answers) |
| Effort for study features | High: audio must bypass AudioManager and be rewritten | Medium | Medium |
| Risk | Upstream moves fast with frequent behaviour-level Metalio changes, the wrapper needs regression each time; Arduino/C++ runtime mixed in | Re-hitting known pitfalls (I²C slow boot, power-off pulse, etc.) | Low: pitfalls known; risk is porting typos, checkable against the original |
| Power control | Constrained by the SDK (no charger, no under-voltage protection, no USB presence) | Fully ours | Fully ours (add charger and under-voltage following the official firmware) |
| Audio timing control | Poor (WAV, task per sentence, amplifier toggled per sentence, no position) | Fully ours | Fully ours |
| Ready-made reader | Can reference / reuse the CrossPoint lineage directly (same SDK, Arduino C++) | None, write our own reader | None; but FreeInkBook (standalone C++17, no Arduino dependency) could be wrapped as a component, lacking ruby / vertical text |
| Fit with the "our own code is C" convention | Poor: C only at the outermost layer, everything below is C++ / Arduino | Good | Good |
| Integration with the existing pure-C study core (container + Opus playback logic) | Needs adaptation via a `std::function` WAV source | Direct | Direct |
| Lock-in | High | None | None (only attribution owed) |

---

## 7. Recommendation `[inference]`

**Choose option 3: ESP-IDF + C, porting the Metalio board logic of FreeInk and the official firmware file by file (file headers keep the MIT attribution and source commit).**

Reasons:

1. The project's two core differentiators — sentence-accurate playback start / shadowing, and low power plus power-off — fall exactly where FreeInk is thinnest or where we must rewrite anyway (AudioManager not reusable, charger and under-voltage unmanaged, USB presence unknown).
2. FreeInk's pure ESP-IDF component cannot drive Metalio today (no `Wire`); using it means pulling in the Arduino runtime or writing our own shims, by which point most of option 3's cost has been paid with an added C++ dependency.
3. The Metalio pitfalls are already answered in FreeInk commits and the official source, so option 3 avoids the detours of option 2; about 3k lines of C in total, manageable.
4. At the reader level, the ruby / vertical text Japanese needs are absent from FreeInkBook `[source] no ruby/writing-mode in FreeInkBook/src`, so the readiness of the CrossPoint lineage is discounted for "Japanese first".

Suggested order of first steps: I²C + TCA9555 power-up sequence → SSD1677 full/fast refresh → keys/touch → power-off pulse + deep sleep → fuel gauge read-only → always-on audio stream + measured start offset → charger (check the cell spec first).

### What would flip this recommendation

| Condition | Flips to |
|---|---|
| Decision to use a ready-made CrossPoint-lineage reader first, defer study features, and accept Arduino + C++ | Option 1 (or simply add features on top of CrossPoint) |
| FreeInk upstream moves input/battery/SD/audio into pure ESP-IDF components and implements `Wire`, and the Metalio docs mark on-device validation complete | Option 1 cost drops sharply; worth re-evaluating |
| Measurement shows the Bluetooth audio module's internal latency is not fixed, so accurate start is impossible | Study features need redesign (independent of the option), but option 3's "full control" advantage shrinks |
| Only horizontal EPUB is needed, no ruby / vertical text | Value of wrapping FreeInkBook as a C++ component rises (still within option 3) |

---

## 8. To verify

- `[to-verify]` DC13/RST18 mapping; panel image quality; touch at the 4 corners and long press on cover keys; SD; fuel gauge readings; RTC; IMU axes; sleep current; power-off / wake (self-listed in the docs, `metalio-eink4-support.md:173-177`).
- `[to-verify]` Bluetooth audio module: fixed latency from I²S write to sound and its jitter; settling time from amplifier enable to audible; module standby current; whether a sleep command exists.
- Check the cell's nominal charge cutoff voltage to set CX25601N VREG; check the CX25601N datasheet for power-on defaults and the default watchdog state.
- Determine whether the unit at hand is the Wi-Fi or the 4G variant (I²C boot timing differs, `[community] 29ae30b`).
