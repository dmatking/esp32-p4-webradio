# Building from source

This covers building and flashing the firmware yourself. If you just want to
run it, flash the prebuilt binary from the [latest release](https://github.com/dmatking/esp32-p4-webradio/releases/latest)
instead — see the [README](README.md).

> For chip-level notes on the P4+C6 combination (esp_hosted init, SDIO, PSRAM,
> errata, etc.) see [esp32-notes](https://github.com/dmatking/esp32-notes).

## Requirements

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) **v5.5.3** with the
  ESP32-P4 toolchain installed.
- The Waveshare ESP32-P4-WIFI6-Touch-LCD-4B board (ESP32-P4 + ESP32-C6).

This board uses **pre-v3 P4 silicon (v1.x)**. The build sets
`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` in `sdkconfig.defaults` so the bootloader
targets the correct chip revision. If you regenerate `sdkconfig` (e.g. after
`idf.py set-target`), delete it first so the default is re-applied:

```bash
rm -f sdkconfig
```

## Build and flash

```bash
source ~/esp/esp-idf-v5.5.3/export.sh

idf.py set-target esp32p4   # only needed once
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

No Wi-Fi credentials are compiled in — the device is provisioned at runtime
through the captive portal (see [First-boot Wi-Fi setup](README.md#first-boot-wi-fi-setup)).

## Producing the merged release binary

The single-file image attached to releases is built with:

```bash
idf.py merge-bin --output p4-webradio-merged.bin
```

It lands in `build/` and is flashable to offset `0x0`.

## Re-provisioning Wi-Fi

Saved Wi-Fi credentials live in NVS. To forget them and re-run first-boot
setup, erase the flash:

```bash
idf.py -p /dev/ttyACM0 erase-flash
```

Then reflash and the `P4-Radio-Setup` access point will come back.

## Architecture

```
main.c          — app state machine, display loop, touch event dispatch
display.c       — MIPI-DSI RGB565, hardware double-buffered (num_fbs=2) flush
wifi.c          — ESP32-C6 hosted Wi-Fi bring-up via the wifi_prov component
radio_browser.c — HTTPS fetch + JSON parse of the Radio Browser API (with retry)
stream.c        — HTTP audio streaming with ICY metadata into a ring buffer
mp3_decoder.c   — libhelix-mp3 decode task, feeds PCM to I2S and the spectrum
audio.c         — ES8311 codec + I2S TX setup
spectrum.c      — 512-point FFT (esp-dsp), 32 log-spaced bands
touch.c         — GT911 direct I2C driver (own task), gesture recognition
font8x16.c      — 8x16 VGA bitmap font, integer-scaled text overlay

components/wifi_prov                      — captive-portal Wi-Fi provisioning (SoftAP + web form, NVS storage)
components/nordesems__esp-captive-portal  — captive-portal DNS/HTTP redirect helper
```

### Notes

- **Display** uses hardware double-buffering (`num_fbs=2`) with an RGB565
  framebuffer; `display_flush()` does a cache sync + `draw_bitmap` + buffer
  swap. This roughly doubled the render rate over the earlier PPA-copy path.
- **Streaming** sends a VLC `User-Agent`, which some stations require. Stations
  that still refuse the connection (HTTP 403 — usually geo-blocking or
  hotlink protection) surface as a "Currently Unavailable" screen.
- **ICY titles** are parsed by matching the closing `';` delimiter, so titles
  containing apostrophes survive; a trailing station URL is stripped.
- **Persistence**: the current station (by URL) and volume are stored in NVS
  under the `radio` namespace and restored on boot.
