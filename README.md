# esp32-p4-webradio

Internet radio player for the [Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4b.htm) board.

> For chip-level notes on the P4+C6 combination (esp_hosted init, SDIO, PSRAM, errata, etc.) see [esp32-notes](https://github.com/dmatking/esp32-notes).

## Features

- Streams MP3 internet radio stations via the Radio Browser API
- 32-band spectrum visualizer on a 720x720 MIPI-DSI display (RGB565, hardware double-buffered)
- GT911 capacitive touch control:
  - Tap left/right third of screen to switch stations
  - Tap center to mute/unmute
  - Swipe up/down to adjust volume
- ICY metadata display (station name, song title)
- Auto-advances to next station if stream dies
- WiFi via ESP32-C6 companion chip (SDIO), set up through a captive-portal page — no credentials compiled into the firmware

## Hardware

| Component | Details |
|-----------|---------|
| MCU | ESP32-P4 (dual-core RISC-V, 400MHz) |
| WiFi | ESP32-C6 companion via SDIO (esp_hosted) |
| Display | 720x720 MIPI-DSI LCD (ST7703 controller) |
| Touch | GT911 capacitive (I2C 0x5D) |
| Audio | ES8311 codec (I2C 0x18) + I2S + onboard PA/speaker |
| PSRAM | 32MB PSRAM for station list and frame buffers |

## Architecture

```
main.c          — app state machine, display loop, touch event dispatch
display.c       — MIPI-DSI RGB565, hardware double-buffered (num_fbs=2) flush
wifi.c          — ESP32-C6 hosted WiFi bring-up via the wifi_prov component
radio_browser.c — HTTPS fetch + JSON parse of Radio Browser API (with retry)
stream.c        — HTTP audio streaming with ICY metadata into ring buffer
mp3_decoder.c   — libhelix-mp3 decode task, feeds PCM to I2S and spectrum
audio.c         — ES8311 codec + I2S TX setup
spectrum.c      — 512-point FFT (esp-dsp), 32 log-spaced bands
touch.c         — GT911 direct I2C driver (own task), gesture recognition
font8x16.c      — 8x16 VGA bitmap font, integer-scaled text overlay

components/wifi_prov              — captive-portal Wi-Fi provisioning (SoftAP + web form, NVS storage)
components/nordesems__esp-captive-portal — captive-portal DNS/HTTP redirect helper
```

## Building

Requires ESP-IDF v5.5.3 with the ESP32-P4 toolchain.

```bash
source ~/esp/esp-idf-v5.5.3/export.sh
idf.py build
idf.py flash monitor
```

No WiFi credentials are compiled into the firmware.

## First-boot WiFi setup

On first boot (or after erasing NVS) the device starts an open Wi-Fi access
point named **`P4-Radio-Setup`** and shows setup instructions on screen:

1. Join the `P4-Radio-Setup` network from a phone or laptop.
2. Open `192.168.4.1` in a browser.
3. Enter your network's SSID and password and submit.

Credentials are stored in NVS and reused on subsequent boots. To re-provision,
erase NVS (`idf.py erase-flash`) and reboot.

## License

Apache-2.0
