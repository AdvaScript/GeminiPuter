# ⚡ GeminiPuter v2.0.1 (Cardputer & Cardputer ADV)

[![GitHub Release](https://img.shields.io/github/v/release/AdvaScript/GeminiPuter?color=38B6FF&style=flat-square)](https://github.com/AdvaScript/GeminiPuter/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg?style=flat-square)](LICENSE)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-ESP32--S3-orange.svg?style=flat-square)](https://platformio.org/)
[![Gemini](https://img.shields.io/badge/Google-Gemini%203.1%20Flash--Lite-blue.svg?style=flat-square)](https://ai.google.dev/)
[![Hardware](https://img.shields.io/badge/M5Stack-Cardputer%20%26%20ADV-purple.svg?style=flat-square)](https://m5stack.com)
[![M5Burner Ready](https://img.shields.io/badge/M5Burner-Merged%20Binary-success.svg?style=flat-square)](https://m5stack.com)

An advanced, interactive firmware transforming the **M5Stack Cardputer** and **Cardputer ADV** into a full-featured pocket AI assistant powered by Google's **Gemini 3.1 Flash-Lite** with native hardware function calling, bilingual Russian/English keyboard layout, live web remote console, and dynamic RGB lighting effects.

> [!NOTE]
> **Fork Attribution**: This project is an advanced fork of the original [GeminiPuter](https://github.com/nishad2m8/GeminiPuter) created by [nishad2m8](https://github.com/nishad2m8). We express our sincere gratitude to the original author for the foundational inspiration!

---

## 🚀 Key Improvements & Comparison

| Feature | Original (`nishad2m8/GeminiPuter`) | Our Fork (`AdvaScript/GeminiPuter` v2.0.1) |
| :--- | :--- | :--- |
| **Hardware Compatibility** | Standard M5Cardputer only | **M5Stack Cardputer** + **Cardputer ADV** |
| **M5Burner Flashing** | Single raw app partition (fails on 0x0) | **Full Merged Image** (`bootloader` + `partitions` + `boot_app0` + `app` @ 0x0) |
| **Flash Mode** | QIO only (fails on some chips) | **Universal DIO (Dual I/O)** compatible with all flash modules |
| **Gemini AI Model** | Hardcoded `gemini-1.5-flash` (Deprecated / 404 error) | **`gemini-3.1-flash-lite`**, `gemini-2.5-flash`, customizable via Web UI |
| **Inference Speed** | Default thinking latency | **Zero-latency thinking mode** (`thinkingBudget: 0`) for near-instant responses |
| **Hardware Function Calling** | ❌ None (pure text assistant) | ✅ **Full Tool Calling**: Screen brightness, WS2812 RGB LED effects, real battery state |
| **RGB LED Integration** | ❌ None | ✅ **11 Built-in animations** + custom JSON animation patterns + visual web mirror |
| **Language & Keyboard** | English only (QWERTY) | 🇷🇺 **Bilingual (English + Russian Cyrillic phonetic layout)** with HUD toast notification |
| **Web Console & Telemetry** | Basic Wi-Fi captive setup portal | 🌐 **Cyberpunk Live Web HUD**: Real-time mirrored transcript, remote keyboard, live telemetry |
| **UI Aesthetics** | Default green text | 🎨 **Sleek Dark Theme**: Electric blue Gemini responses (`#38B6FF`), right-aligned user bubbles |
| **Watchdog & Stability** | Watchdog resets on long answers | 🛡️ FreeRTOS thread isolation, mutex guards, feed loops preventing WDT panics |
| **Easter Eggs** | ❌ None | 🤢 Built-in IMU Shake / `/shake` Easter egg |

---

## ✨ Features Breakdown

### 🧠 Gemini 3.1 & Hardware Control (Function Calling)
Gemini can directly control your Cardputer hardware using Google Function Declarations:
- **Screen Brightness**: `"Set screen brightness to 20%"` or `"Dim the display"`.
- **RGB LED Lamp**: `"Turn the lamp red"`, `"Switch to rainbow effect"`, `"Blink cyan at 50% brightness"`, `"Turn off the LED"`.
- **Custom Patterns**: Ask Gemini to generate custom blinking patterns or moods.
- **Battery Status**: `"How much battery is left?"` (queries the internal AXP/ADC power sensor).

### ⌨️ Bilingual Russian & English Keyboard
- Switch layouts instantly by pressing **`Fn + Backspace`** or **`Fn + Enter`**.
- An on-screen translucent toast indicator **`[ РУС ]`** / **`[ ENG ]`** displays the active mode.
- Rendered with an embedded high-definition Cyrillic font (`rubik_14_cy`).

### 🌐 Cyberpunk Web Interface & Remote Keyboard
Connect to `http://<cardputer-ip>/` from any computer, tablet, or smartphone on the same network:
- **Live Mirrored Console**: Watch conversation turns scroll in real time with custom message bubbles.
- **Remote Keyboard**: Type questions using your desktop keyboard with command history (`Up`/`Down` arrows).
- **Quick Action Chips**: One-click shortcuts for lighting, brightness, battery, and testing.
- **Hardware Telemetry HUD**: Live battery indicator, Wi-Fi RSSI signal meter, uptime, free heap memory, and simulated RGB lamp status.
- **Web Settings**: Change API keys, Wi-Fi credentials, or switch Gemini models on the fly without reflashing.

---

## 🎮 Cardputer Controls

| Key Combination | Action |
| :--- | :--- |
| **`Tab`** | Toggle focus between Chat Input and AI Response transcript |
| **`Fn + ;`** / **`Fn + .`** | Scroll transcript Up / Down |
| **`Enter`** | Send message to Gemini |
| **`Fn + Backspace`** or **`Fn + Enter`** | Toggle Russian / English keyboard layout |
| **`Ctrl + L`** | Clear transcript |
| **`/shake`** or **`/puke`** | Trigger the accelerometer Easter egg |

---

## 📥 Installation & Flashing

### Option 1: Flashing with M5Burner or esptool (Recommended)
Download the latest `GeminiPuter.bin` (or `GeminiPuter-v2.0.1-merged-M5Burner.bin`) from [GitHub Releases](https://github.com/AdvaScript/GeminiPuter/releases).

This is a **complete merged image** packaged starting at offset `0x0000` containing:
- `0x0000`: ESP32-S3 Bootloader
- `0x8000`: Partition Table
- `0xe000`: Boot App0 (OTA selector)
- `0x10000`: Application Firmware

Flash using **esptool.py**:
```bash
esptool.py --chip esp32s3 write_flash 0x0 GeminiPuter.bin
```
Or burn directly via **M5Burner** by uploading `GeminiPuter.bin` as the firmware binary!

### Option 2: Build from Source with PlatformIO
1. Clone this repository:
   ```bash
   git clone https://github.com/AdvaScript/GeminiPuter.git
   cd GeminiPuter/PIO
   ```
2. Build and upload firmware:
   ```bash
   pio run -t upload
   ```

---

## ⚙️ Initial Configuration

1. On first boot, Cardputer creates or connects to your Wi-Fi network.
2. Follow the on-screen instructions or connect to `http://<device-ip>/` in your web browser.
3. Obtain a free API key from [Google AI Studio](https://aistudio.google.com/).
4. Enter your API key into the web configuration panel or `config.json` on the SD card.
5. Save and reboot — your GeminiPuter is ready!

---

## 🔗 Fork Status & Upstream Link

- **Original Project**: [nishad2m8/GeminiPuter](https://github.com/nishad2m8/GeminiPuter)
- **Author of Original**: [Nishad](https://github.com/nishad2m8)
- **Fork Maintainer**: [AdvaScript](https://github.com/AdvaScript)

> [!TIP]
> **GitHub Fork Tree Attachment**: Because this repository was initially created as a standalone project, it is not attached automatically to the upstream fork tree by GitHub. If you manage this repo and wish it to be visible under upstream's "Forks" tab (`https://github.com/nishad2m8/GeminiPuter/network/members`), you can request [GitHub Support](https://support.github.com) to link this repository as a fork of `nishad2m8/GeminiPuter`.

---

## 📄 License

This project is distributed under the **MIT License**. See [LICENSE](LICENSE) for details.
