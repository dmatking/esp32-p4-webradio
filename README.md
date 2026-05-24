# ESP32-P4 Web Radio

A touchscreen internet radio for the [Waveshare ESP32-P4-WIFI6-Touch-LCD-4B](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4b.htm). Streams MP3 radio stations from the [Radio Browser](https://www.radio-browser.info/) directory and shows a live 32-band spectrum visualizer with the station name and current song.

No Wi-Fi credentials are baked into the firmware — you set up your network on first boot through a phone or laptop.

## Flash the prebuilt binary

The easiest way to get started. Grab `p4-webradio-vX.Y.Z.bin` from the [latest release](https://github.com/dmatking/esp32-p4-webradio/releases/latest) — it's a complete image (bootloader + partition table + app).

1. Install [esptool](https://docs.espressif.com/projects/esptool/) if you don't have it: `pip install esptool`
2. Connect the board over USB.
3. Flash it to offset `0x0`:

   ```bash
   esptool.py --chip esp32p4 write_flash 0x0 p4-webradio-v1.0.0.bin
   ```

   (Pass `-p /dev/ttyACM0` or your port if esptool can't find the board.)

That's it — the radio reboots into the Wi-Fi setup below.

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

## Hardware

| Component | Details |
|-----------|---------|
| MCU | ESP32-P4 (dual-core RISC-V, 400 MHz) |
| Wi-Fi | ESP32-C6 companion via SDIO (esp_hosted) |
| Display | 720×720 MIPI-DSI LCD (ST7703 controller) |
| Touch | GT911 capacitive |
| Audio | ES8311 codec + I2S + onboard speaker |
| PSRAM | 32 MB (station list and frame buffers) |

## Building from source

See **[BUILDING.md](BUILDING.md)** for toolchain setup, build/flash commands,
the project architecture, and developer notes.

## License

Apache-2.0
