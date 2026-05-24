# ESP32-P4 Web Radio

A touchscreen internet radio for ESP32-P4 boards. Streams MP3 radio stations
from the [Radio Browser](https://www.radio-browser.info/) directory and shows a
live 32-band spectrum visualizer with the station name and current song.

No Wi-Fi credentials are baked into the firmware — you set up your network on
first boot through a phone or laptop.

## Supported boards

| Board | Display | Touch | Audio |
|-------|---------|-------|-------|
| [M5Stack Tab5](https://docs.m5stack.com/en/products/sku/k145) | 1280×720 landscape (ST7123) | ST7123 | ES8388 |
| [Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4b.htm) | 720×720 (ST7703) | GT911 | ES8311 |

Both use an ESP32-C6 co-processor for Wi-Fi (via `esp_hosted` over SDIO). Pick
the matching release binary for your board.

## Flash the prebuilt binary

The easiest way to get started. From the
[latest release](https://github.com/dmatking/esp32-p4-webradio/releases/latest),
download the binary for your board:

- `p4-webradio-tab5-*.bin`
- `p4-webradio-waveshare-*.bin`

Each is a complete single image — bootloader, app, **and** the Wi-Fi
co-processor firmware all in one file.

1. Install [esptool](https://docs.espressif.com/projects/esptool/) if needed: `pip install esptool`
2. Connect the board over USB.
3. Flash it to offset `0x0`:

   ```bash
   esptool.py --chip esp32p4 write_flash 0x0 p4-webradio-<board>-<version>.bin
   ```

   (Add `-p /dev/ttyACM0` or your port if esptool can't find the board.)

**First boot takes ~30 seconds.** If your board shipped with old Wi-Fi
co-processor firmware (most do), the radio updates it automatically on that
first boot — you don't need to do anything. After that it goes straight to the
Wi-Fi setup below.

## First-boot Wi-Fi setup

The first time it runs (or after erasing the device), the radio starts its own
Wi-Fi network and shows setup instructions on screen:

1. On your phone or laptop, join the Wi-Fi network named **`P4-Radio-Setup`**.
2. Open **`192.168.4.1`** in a browser.
3. Enter your home network's name and password.
4. Optionally choose which stations to load:
   - **Country code** — ISO 2-letter (e.g. `US`, `GB`, `DE`). Leave blank for worldwide.
   - **State / region** — e.g. `Texas`. Optional; leave blank for the whole country.
   - **Number of stations** — 1–60 (default 30).

   Leave these blank to use the built-in defaults (top US/Texas stations).
5. Submit.

The radio saves your settings and connects automatically on every boot after
that. To change your network or station selection, erase the device and start
over (see [BUILDING.md](BUILDING.md#re-provisioning-wi-fi)).

## Touch controls

| Gesture | Action |
|---------|--------|
| Tap the **left** third of the screen | Previous station |
| Tap the **right** third of the screen | Next station |
| Tap the **center** | Mute / unmute |
| Swipe **up** | Volume up |
| Swipe **down** | Volume down |

The radio remembers your last station and volume across reboots. If a station
won't play (some block out-of-region listeners), it shows **Currently
Unavailable** — just tap to the next one.

## About the Wi-Fi co-processor firmware

The ESP32-P4 has no built-in radio; an ESP32-C6 handles Wi-Fi. Boards commonly
ship with an old C6 firmware (v0.0.0) that works for Wi-Fi but lacks Bluetooth.
The prebuilt binaries bundle a current C6 firmware (v2.12.3) and flash it to the
co-processor automatically on first boot — so you get an up-to-date, BT-capable
radio with nothing extra to do. Subsequent boots detect it's already current and
skip the update.

(If you build from source, there's a one-time step to seed that firmware — see
[BUILDING.md](BUILDING.md).)

## Building from source

See **[BUILDING.md](BUILDING.md)** for toolchain setup, per-board build/flash
commands, the C6 firmware step, the project architecture, and developer notes.

## License

Apache-2.0
