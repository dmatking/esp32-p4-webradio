# p4-radio

Internet radio player for the [Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4b.htm) board.

## Features

- Streams MP3 internet radio stations via the Radio Browser API
- 32-band spectrum visualizer with peak hold, rendered at 30fps on 720x720 MIPI-DSI display
- GT911 capacitive touch control:
  - Tap left/right third of screen to switch stations
  - Tap center to mute/unmute
  - Swipe up/down to adjust volume
- ICY metadata display (station name, song title)
- Auto-advances to next station if stream dies
- WiFi via ESP32-C6 companion chip (SDIO)

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
display.c       — MIPI-DSI + PPA SRM double-buffered flush
wifi.c          — ESP32-C6 hosted WiFi init
radio_browser.c — HTTPS fetch + JSON parse of Radio Browser API
stream.c        — HTTP audio streaming with ICY metadata into ring buffer
mp3_decoder.c   — libhelix-mp3 decode task, feeds PCM to I2S and spectrum
audio.c         — ES8311 codec + I2S TX setup
spectrum.c      — 512-point FFT (esp-dsp), 32 log-spaced bands
touch.c         — GT911 direct I2C driver with gesture recognition
font8x16.c      — 8x16 VGA bitmap font for text overlay
```

## Building

Requires ESP-IDF v5.5.1 with the ESP32-P4 toolchain.

```bash
source ~/esp/esp-idf-v5.5.1/export.sh
idf.py build
idf.py flash monitor
```

WiFi credentials are loaded from `~/.esp_creds` via `SDKCONFIG_DEFAULTS` — never hardcoded. Create the file with:

```
CONFIG_ESP_WIFI_SSID="YourSSID"
CONFIG_ESP_WIFI_PASSWORD="YourPassword"
```

## License

Apache-2.0
