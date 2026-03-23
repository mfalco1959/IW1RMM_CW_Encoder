# IW1RMM CW Encoder

**Firmware v7.1.5 — IW1RMM (Mauri)**

WinKey WK3-compliant Morse code keyer for the ESP32-2432S028R (CYD), with ILI9341 320×240 touchscreen display.
Based on the original firmware by VK2IDL, extended and adapted for the CYD platform.

---

## Hardware

### Schematic

![Schematic](IW1RMM_CW_Encoder/images/schematic.jpg)

### Paddle and straight key wiring

![Plug wiring — paddle and straight key — 2N2222 transistor TO-92 and TO-18](IW1RMM_CW_Encoder/images/hardware_connections.jpg)

### Internal wiring

![Internal wiring](IW1RMM_CW_Encoder/images/internal_wiring.jpg)

### Overview

![CYD in case with paddle connected](IW1RMM_CW_Encoder/images/device_overview.jpg)

---

## Features

- WinKey WK3 via USB Serial (1200 baud)
- K3NG ASCII command set via Serial and BLE Nordic UART
- LVGL touchscreen UI (5 tabs)
- Keyer modes: Iambic A/B, Bug, Auto, Ultimatic
- SD card playback with CSV logging
- QRSS / HSCW / Beacon / Contest
- Interactive 4-point touch calibration
- Clock with GMT offset, optional DS3231 RTC
- Farnsworth, Prosign, DAH ratio, Spacing weight
- K3NG memories A–Z (NVS)
- 6 preset messages (NVS)

---

## Screenshots

### Tab Morse 1

![Tab Morse 1](IW1RMM_CW_Encoder/images/tab_morse1.jpg)

### Keyboard — long press

![Keyboard — long press on CQ, NAME keys etc.](IW1RMM_CW_Encoder/images/tab_keyboard.jpg)

### Tab Morse 2

![Tab Morse 2](IW1RMM_CW_Encoder/images/tab_morse2.jpg)

### Tab SD

![Tab SD](IW1RMM_CW_Encoder/images/tab_sd.jpg)

### Tab Settings — default state

![Tab Settings — default state](IW1RMM_CW_Encoder/images/tab_settings.jpg)

### Tab Settings — active buttons

![Tab Settings — active buttons](IW1RMM_CW_Encoder/images/tab_settings_active.jpg)

---

## BLE

The firmware exposes a Nordic UART Service (NUS) with device name **VK2IDL_Morse**.  
It supports the same K3NG command set available via Serial.

### K3NG commands via BLE — smartphone screenshot

![Smartphone screenshot — K3NG commands via BLE](IW1RMM_CW_Encoder/images/ble_smartphone.jpg)

---

## Pinout ESP32-2432S028R (CYD)

| GPIO | Function | Notes |
|------|----------|-------|
| 27 | KEY / PTT output | Keyer output |
| 26 | Sidetone DAC | Audio 300–1200 Hz |
| 22 | DIT input | Paddle |
| 21 | DAH input | Paddle |
| 5 | SD_CS | SPI SD card |
| 23 | SD_MOSI | SPI SD card |
| 19 | SD_MISO | SPI SD card |
| 18 | SD_SCK | SPI SD card |
| 2 | TFT_RS | ILI9341 display |
| 15 | TFT_CS | ILI9341 display |
| 4 | XPT2046 CS | Touch |
| 36 | XPT2046 IRQ | Touch interrupt |

---

## Dependencies (Arduino libraries)

| Library | Version | Author |
|---------|---------|--------|
| lvgl | **8.4.0** | LVGL |
| TFT_eSPI | **2.5.43** | Bodmer |
| XPT2046_Touchscreen | **1.4** | Paul Stoffregen |
| DacESP32 | **2.1.2** | Thomas Jentzsch |
| Preferences (NVS) | built-in | Espressif |
| BLE (BLEDevice/BLEServer/BLEUtils/BLE2902) | built-in | Espressif |

> ⚠️ Tested with ESP32 Arduino Core **3.x**. The BLE and DacESP32 libraries require Core 3.x — do not use version 2.x.

---

## Installation

1. Clone the repository
```bash
git clone https://github.com/TUO_USER/IW1RMM_CW_Encoder.git
```
2. Copy `include/lv_conf.h` into the sketch folder (same directory as `IW1RMM_CW_Encoder.ino`)
3. Configure TFT_eSPI `User_Setup.h` (see `docs/User_Setup_example.h`)
4. Compile with Arduino IDE — board: **ESP32 Dev Module**  
   In **Tools**, set:
   - Flash Mode: **DIO**
   - Flash Frequency: **40MHz**
   - Partition Scheme: **Huge APP (3MB No OTA/1MB SPIFFS)**
   - Upload Speed: **115200**
5. Upload via Arduino IDE — due to the large sketch size, **upload will take several minutes**.

> ⚠️ **DIO + 40MHz are mandatory.** CYD boards often use ZBIT ZB25VQ32 flash chips that are sensitive to QIO or 80MHz settings — using QIO or 80MHz will cause upload failures or boot loops.

> 💡 **Upload speed:** 115200 baud is the safe recommended value. If you plan to upload only occasionally, you may try increasing the speed (e.g. 460800 or 921600) to reduce upload time, but stability depends on your USB cable and PC driver.

---

## K3NG Main Commands

| Command | Function |
|---------|----------|
| `\Snn` | Set speed (WPM) |
| `\Fnnnn` | Set sidetone frequency (Hz) |
| `\Ln` | Set volume level (0–4) |
| `\R nnn` | DAH ratio × 100 (e.g. \R300 = 3.0) |
| `\W nnn` | Spacing weight × 100 (e.g. \W100 = 1.0) |
| `\Y nn` | Farnsworth WPM (0 = disable) |
| `\Q n` | QRSS (0/3/6/10/30 seconds per dit) |
| `\T` | TUNE — continuous carrier |
| `\J` | Reset touch calibration |
| `\Z DDMMAAAAHHmmSS` | Set clock |
| `\G ±n` | GMT offset |
| `\PA text` | Save K3NG memory (A–Z) |
| `\MA` | Send K3NG memory A |
| `\P` | List all memories |
| `\?` | Full help |

---

## Version

| Version | Date | Notes |
|---------|------|-------|
| v7.1.5 | 2026-03 | Code comments optimised, reduced code lines. Minor text adjustments. |
| v7.1.4 | 2026-03 | Touch calibration, prosign, DS3231, extended K3NG |

---

## Credits

- Original firmware: **VK2IDL**
- Modifications, extensions and CYD adaptation: **IW1RMM (Mauri)**, 2025–2026

---

## License

GPL v3 — see `LICENSE` file
