# IW1RMM CW Encoder

**A complete Morse code keyer for the ESP32 CYD — firmware v7.1.4**

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Platform: ESP32](https://img.shields.io/badge/Platform-ESP32-green.svg)](https://www.espressif.com/en/products/socs/esp32)
[![Hardware: CYD](https://img.shields.io/badge/Hardware-ESP32--2432S028R-yellow.svg)](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display)

**Developed by Mauri IW1RMM, 2025–2026 — Based on original firmware by VK2IDL**

---

## Overview

The IW1RMM CW Encoder turns a €10 ESP32 touchscreen board into a full-featured
Morse code keyer. Plug in your paddle and key — everything else is on the touchscreen.

Hardware: **ESP32-2432S028R** (CYD — Cheap Yellow Display), ILI9341 320×240
colour touchscreen, XPT2046 resistive touch, built-in BLE, MicroSD slot.

---

## Features

### Keying & Paddle
- Iambic A, Iambic B, Manual (straight key), Auto (electronic bug), Ultimatic
- Real-time character decoding: every transmitted character appears on the scrolling display
- Paddle swap (DIT/DAH) in one tap

### Touchscreen Interface
- LVGL-based UI — 5 tabs: Morse 1, Morse 2, SD, Settings, Spare
- Interactive 4-point touch calibration on first boot, stored in NVS *(NEW v7.1.4)*
- Scrolling text bars show transmitted text live
- Virtual keyboard for message editing directly on screen

### Speed & Timing
- 5 to 100 WPM, adjustable on screen
- DAH ratio (2.0–4.5×), spacing weight, Farnsworth character spacing
- All values saved to non-volatile memory, restored on every boot

### Preset Messages
- 6 editable message slots: CQ, Name, Test, Ant/Rig, RST, Free
- Send with one tap, edit via on-screen keyboard, stored permanently in NVS
- Messages can be sent individually or chained

### SD Card Player
- Load any `.txt` file from MicroSD and transmit in Morse
- PLAY / PAUSE / STOP — text scrolls live on display during transmission
- Session logging to CSV with timestamp, WPM, frequency

### Audio
- Internal ESP32 DAC sidetone — 300 to 1200 Hz, 4 volume levels
- No external audio hardware required

### Wireless BLE Control
- Nordic UART BLE service — control from any Android phone or PC *(FIX v7.1.3)*
- K3NG ASCII command set over BLE: set speed, play memories, query status
- Always available regardless of operating mode

### Beacon & Contest
- Beacon: automatic CQ at configurable intervals (10–600 s)
- Contest: auto-incrementing serial number, saved to NVS

### Special Modes
- HSCW (60–100+ WPM) for meteor scatter and aurora
- QRSS (3s, 6s, 10s, 30s per DIT) for low-power propagation experiments
- Full prosign support: `<AR>` `<SK>` `<BT>` `<KN>` `<AS>` `<SN>`
- Built-in clock — optional DS3231 RTC for persistent timekeeping

### PC Integration (optional)
- **WinKey 2.3** binary protocol at 1200 baud — for N1MM+, Win-Test, DXLog, Logger32
- **K3NG serial** ASCII commands at 115200 baud via USB

### Architecture
- FreeRTOS: morseTask on Core 0, LVGL on Core 1
- 1 kHz hardware timer for sub-millisecond keying accuracy
- Non-blocking design throughout — no `delay()` in timing-critical paths

---

## Hardware

| Component | Detail |
|---|---|
| Board | ESP32-2432S028R (CYD) |
| MCU | ESP32-D0WD dual-core 240 MHz · 520 KB RAM · 4 MB flash |
| Display | ILI9341 2.8" TFT 320×240 · SPI (VSPI) |
| Touch | XPT2046 resistive · SPI (HSPI) · 4-point calibrated |
| KEY / PTT output | GPIO 27 · shared pin · HIGH = active |
| Sidetone | GPIO 25 DAC · 300–1200 Hz |
| Left paddle (DIT) | GPIO 35 |
| Right paddle (DAH) | GPIO 22 |
| SD card | HSPI · GPIO 18 / 19 / 23 / 5 |
| Power | USB-C 5V · no external supply needed |

---

## Installation

### 1. Install the ESP32 board package

In Arduino IDE: **File → Preferences → Additional boards manager URLs**, add:

```
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```

Then **Tools → Board → Boards Manager** → search `esp32` → install **esp32 by Espressif Systems**.

> **Requires ESP32 Arduino Core 3.x** — DacESP32 and BLE libraries require Core 3.x. Do not use 2.x.

### 2. Install required libraries

In Arduino IDE: **Sketch → Include Library → Manage Libraries**

| Library | Version | Author | How to install |
|---|---|---|---|
| lvgl | **8.4.0** | LVGL | Library Manager → search `lvgl` |
| TFT_eSPI | **2.5.43** | Bodmer | Library Manager → search `TFT_eSPI` |
| XPT2046_Touchscreen | **1.4** | Paul Stoffregen | Library Manager → search `XPT2046` |
| DacESP32 | **2.1.2** | Thomas Jentzsch | Library Manager → search `DacESP32` |
| SD | built-in | Arduino | Built-in — no install needed |
| SPI | built-in | Arduino | Built-in — no install needed |
| Preferences | built-in | Espressif | Built-in with ESP32 Core |
| BLEDevice / BLEServer / BLEUtils / BLE2902 | built-in | Espressif | Built-in with ESP32 Core |
| esp_bt.h | built-in | Espressif | Built-in with ESP32 Core (ESP-IDF) |

### 3. Configure TFT_eSPI

Copy `docs/User_Setup_example.h` to your TFT_eSPI library folder
(typically `Documents/Arduino/libraries/TFT_eSPI/`) and rename it `User_Setup.h`.

```cpp
#define ILI9341_DRIVER
#define TFT_MISO  12    // ← must be defined, do NOT comment out
#define TFT_MOSI  13
#define TFT_SCLK  14
#define TFT_CS    15
#define TFT_DC     2
#define TFT_RST   -1
#define TOUCH_CS  -1
#define SPI_FREQUENCY  40000000
```

> **Important**: `TFT_MISO 12` must be defined and not commented out — the SD card will fail to initialise otherwise.

### 4. Copy lv_conf.h

Copy `include/lv_conf.h` to your LVGL library folder
(typically `Documents/Arduino/libraries/lvgl/`), replacing the existing file.
This enables the fonts used by the UI — without it the sketch will not compile.

### 5. Arduino IDE upload settings

| Setting | Value |
|---|---|
| Board | ESP32 Dev Module |
| Flash Mode | **DIO** |
| Flash Frequency | **40 MHz** |
| Partition Scheme | Default 4MB with spiffs |
| Upload Speed | 115200 |

> **DIO + 40 MHz are mandatory.** CYD boards often use ZBIT ZB25VQ32 flash chips
> that are sensitive to faster or QIO settings — using QIO or 80 MHz will cause
> upload failures or boot loops.

### 6. Upload

Open `IW1RMM_CW_Encoder/IW1RMM_CW_Encoder.ino` in Arduino IDE, select the correct
COM port under **Tools → Port**, click **Upload**.

On first boot, the **touch calibration wizard** starts automatically — touch each
of the 4 corner crosshairs as instructed. Calibration is saved permanently and
will not repeat on subsequent boots.

### Alternative: esptool

```bash
python -m esptool --chip esp32 --port COM4 --baud 115200 \
  --before default-reset --after hard-reset \
  write-flash -z --flash-mode dio --flash-freq 40m --flash-size detect \
  0x10000 IW1RMM_CW_Encoder_v714.bin
```

Replace `COM4` with your port (Linux: `/dev/ttyUSB0`, Mac: `/dev/cu.usbserial-*`).

---

## K3NG Serial / BLE Commands (selection)

| Command | Function |
|---|---|
| `\S nn` | Set speed (WPM) |
| `\W nn` | Set weight |
| `\F nn` | Set Farnsworth WPM |
| `\Y nn` | Set sidetone frequency (Hz) |
| `\M x text` | Store memory slot A–Z |
| `\P x` | Play memory slot A–Z |
| `\Z hh:mm:ss` | Set clock |
| `\J` | Reset touch calibration |
| `\X` | Toggle HSCW mode |
| `\H` | Help — full command list |
| `\V` | Firmware version |

Full reference: see `docs/` folder.

---

## Version History

| Version | Changes |
|---|---|
| v7.1.4 | Interactive 4-point touch calibration, NVS namespace `touch_cal`, K3NG commands `\J \Y \X \E` |
| v7.1.3 | BLE fix: device name now visible on Android scan |
| v7.1.2 | MANUAL mode fix, prosign display, `mySet[]` extended to 121 elements (full ITU) |
| v7.1.1 | FreeRTOS multi-task refactor, native BLEDevice |
| v6.9.0 | Base version — VK2IDL original firmware, IW1RMM modifications begin |

---

## 3D Printable Case

The project includes a complete 3D printable enclosure for the CYD board.
Files are in the `hardware/` folder — print with PLA or PETG, 0.2 mm layer height.

| File | Part |
|---|---|
| [Case-Front_Panel.stl](hardware/Case-Front_Panel.stl) | Front panel |
| [Case-Rear_Panel.stl](hardware/Case-Rear_Panel.stl) | Rear panel |
| [Speaker-Cap.stl](hardware/Speaker-Cap.stl) | Speaker cap |
| [Washer.stl](hardware/Washer.stl) | Washer |

> GitHub renders STL files in 3D — click any file above to preview it in the browser.

---

## Credits

- **Original firmware**: VK2IDL — foundational CW encoder design
- **Development & extensions**: Mauri IW1RMM, 2025–2026
- **K3NG keyer**: Anthony Good K3NG — [github.com/k3ng/k3ng_cw_keyer](https://github.com/k3ng/k3ng_cw_keyer)
- **WinKey protocol**: K1EL — [k1el.com](https://www.k1el.com)
- **LVGL**: [lvgl.io](https://lvgl.io)
- **CYD community**: Brian Lough — [ESP32 Cheap Yellow Display](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display)

---

## License

This project is licensed under the **GNU General Public License v3.0**.
See [LICENSE](LICENSE) for the full text.

---

*73 de IW1RMM — Mauri, Savona, Italy*
