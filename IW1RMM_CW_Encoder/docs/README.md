# IW1RMM CW Encoder

**WinKey WK3-compliant Morse Code Keyer for ESP32 CYD**

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Platform: ESP32](https://img.shields.io/badge/Platform-ESP32-green.svg)](https://www.espressif.com/en/products/socs/esp32)
[![Hardware: CYD](https://img.shields.io/badge/Hardware-ESP32--2432S028R-yellow.svg)](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display)

**Firmware v7.1.4 — Developed by Mauri IW1RMM, 2025–2026**

> Based on the original VK2IDL CW Encoder firmware — full credit to VK2IDL for the foundational work.

---

## Overview

The IW1RMM CW Encoder is a feature-rich Morse code keyer running on the
**ESP32-2432S028R** (known as CYD — Cheap Yellow Display), a compact and
affordable ESP32 board with an integrated ILI9341 320×240 colour touchscreen.

It implements the **WinKey WK3 protocol** for seamless integration with
logging software (Win-Test, N1MM+, HRD, etc.), and the **K3NG ASCII command
set** over BLE for wireless control from Android devices.

---

## Features

### Keying Modes
- Iambic A and Iambic B
- Bug (semi-automatic)
- Ultimatic
- Auto (single-lever/mono-leva, electronic bug behaviour)
- Straight key

### Protocols
- **WinKey WK3** — USB Serial binary protocol at 1200 baud (compatible with Win-Test, N1MM+, HRD, DX4WIN, WriteLog)
- **K3NG ASCII command set** — via BLE Nordic UART (Android / PC)
- Both protocols coexist simultaneously; BLE is always available regardless of WinKey mode

### User Interface
- LVGL-based touchscreen UI — 5 tabs
- Interactive 4-point touch calibration at first boot (saved to NVS)
- Real-time WPM, mode, and TX status display
- Message editor for 6 quick-send messages

### Speed & Timing
- Speed range: 5–100+ WPM
- HSCW support (60–100+ WPM) for meteor scatter and aurora
- QRSS (3, 6, 12, 30, 60 second dots)
- Farnsworth character spacing
- Adjustable dit/dah weight and ratio

### Memory & Storage
- 6 message slots (touchscreen buttons: CQ, Name, Test, Ant/Rig, RST, Free)
- K3NG memories A–Z stored in NVS (non-volatile)
- SD card playback for long text files

### Other
- Beacon mode with programmable interval
- Contest serial number auto-increment
- Prosign support: `<AR>` `<SK>` `<BT>` `<KN>` `<AS>` `<SN>` `<HH>` `<CL>` `<CT>` `<KA>`
- Sidetone via DAC (GPIO 25)
- FreeRTOS multitasking (MorseTask on Core 0, LVGL on Core 1)

---

## Hardware

| Component | Details |
|---|---|
| Board | ESP32-2432S028R (CYD) |
| MCU | ESP32-D0WD dual-core 240 MHz |
| Display | ILI9341 320×240 TFT, SPI |
| Touch | XPT2046 resistive, SPI |
| Flash | 4 MB (ZBIT ZB25VQ32 or similar) |
| KEY/PTT output | GPIO 27 (shared, HIGH = active) |
| Sidetone | GPIO 25 (DAC) |
| SD Card | HSPI bus (GPIO 18/19/23/5) |
| Left paddle | GPIO 12 |
| Right paddle | GPIO 13 |

> **Note**: KEY and PTT share the same pin (GPIO 27). No separate PTT hardware
> is required; PTT lead/tail timing is not applicable.

---

## Software Dependencies (Arduino Libraries)

| Library | Notes |
|---|---|
| LVGL ≥ 8.3 | UI framework |
| TFT_eSPI | Display driver |
| XPT2046_Touchscreen | Touch driver |
| ESP32 BLE Arduino | BLE Nordic UART |
| ESP32 Preferences | NVS storage |
| SD (built-in) | SD card |
| DacESP32 | Sidetone DAC (requires ESP32 Core 3.x) |
| FreeRTOS | Built into ESP32 Arduino Core |

---

## Installation

### 1. Clone the repository

```bash
git clone https://github.com/IW1RMM/IW1RMM_CW_Encoder.git
```

### 2. Configure TFT_eSPI

Copy `docs/User_Setup_example.h` to your TFT_eSPI library folder as `User_Setup.h`.
Key settings for the CYD:

```cpp
#define ILI9341_DRIVER
#define TFT_MISO  12
#define TFT_MOSI  13
#define TFT_SCLK  14
#define TFT_CS    15
#define TFT_DC     2
#define TFT_RST   -1
#define TOUCH_CS  -1
#define SPI_FREQUENCY  40000000
```

### 3. Configure lv_conf.h

Copy `include/lv_conf.h` to your LVGL library folder.

### 4. Arduino IDE settings

| Setting | Value |
|---|---|
| Board | ESP32 Dev Module |
| Flash Mode | DIO |
| Flash Frequency | 40MHz |
| Partition Scheme | Default 4MB with spiffs |
| Upload Speed | 115200 |

> **Important**: Flash Mode DIO and 40 MHz are required for stability with
> ZBIT flash chips common on CYD boards. Do not use QIO or 80 MHz.

### 5. Upload

Via Arduino IDE (Sketch → Upload), or directly with esptool:

```bash
python -m esptool --chip esp32 --port COM4 --baud 115200 \
  --before default-reset --after hard-reset \
  write-flash -z --flash-mode dio --flash-freq 40m --flash-size detect \
  0x10000 IW1RMM_CW_Encoder_v714.bin
```

---

## WinKey WK3 Compatibility

Tested with:
- **WKdemo.exe** (K1EL)
- **WK3demo.exe** (K1EL)
- Win-Test
- N1MM+ Logger

Serial port settings: **1200 baud, 8N1**

The firmware auto-switches to 1200 baud when WinKey mode is activated,
and returns to 115200 baud when returning to K3NG mode.

---

## K3NG BLE Commands (selection)

| Command | Function |
|---|---|
| `\S nn` | Set speed (WPM) |
| `\W nn` | Set weight |
| `\F nn` | Set Farnsworth WPM |
| `\M x text` | Store memory (A–Z) |
| `\P x` | Play memory (A–Z) |
| `\H` | Help / command list |
| `\V` | Firmware version |
| `\J` | Reset touch calibration |
| `\Y` | Set sidetone frequency |
| `\X` | Toggle HSCW mode |
| `\E` | Toggle extended display |
| `\Z hh:mm:ss` | Set clock |

Full command reference: see `docs/` folder.

---

## Touch Calibration

On first boot (or after `\J` command), the device enters interactive
4-point touch calibration mode. Follow the on-screen prompts.
Calibration data is stored in NVS under the `touch_cal` namespace and
survives power cycles.

---

## Version History

| Version | Changes |
|---|---|
| v7.1.4 | Touch calibration (4-point, NVS), K3NG commands `\J \Y \X \E` |
| v7.1.3 | BLE fix: device name `VK2IDL_Morse` now visible on Android scan |
| v7.1.2 | MANUAL mode fix, prosign display, mySet[] extended to 121 elements |
| v7.1.1 | FreeRTOS multi-task refactor, BLEDevice native (non-NimBLE) |
| v6.9.0 | Base version — VK2IDL original firmware, IW1RMM modifications begin |

---

## Credits

- **Original firmware**: VK2IDL — foundational CW encoder design
- **Development & extensions**: Mauri IW1RMM, 2025–2026
- **WinKey protocol**: K1EL — [k1el.com](https://www.k1el.com)
- **K3NG keyer**: Anthony Good K3NG — [github.com/k3ng/k3ng_cw_keyer](https://github.com/k3ng/k3ng_cw_keyer)
- **LVGL**: [lvgl.io](https://lvgl.io)
- **CYD community**: Brian Lough — [ESP32 Cheap Yellow Display](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display)

---

## License

This project is licensed under the **GNU General Public License v3.0**.
See [LICENSE](LICENSE) for the full text.

---

*73 de IW1RMM — Mauri, Genova, Italy*
