# Building from source

This covers building and flashing the firmware yourself. If you just want to
run it, flash the prebuilt binary from the [latest release](https://github.com/dmatking/esp32-p4-webradio/releases/latest)
instead — see the [README](README.md).

> For chip-level notes on the P4+C6 combination (esp_hosted init, SDIO, PSRAM,
> errata, console config, etc.) see [esp32-notes](https://github.com/dmatking/esp32-notes).

## Requirements

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) **v5.5.3** with the
  ESP32-P4 toolchain.
- One of the supported boards (both are ESP32-P4 + ESP32-C6):
  - M5Stack Tab5 (720×1280 panel run landscape, ST7123, ES8388) — default
  - Waveshare ESP32-P4-WIFI6-Touch-LCD-4B (720×720, ST7703, GT911, ES8311)

These boards use **pre-v3 P4 silicon (v1.x)**. The build sets
`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` in `sdkconfig.defaults`. If you regenerate
`sdkconfig` (e.g. after `idf.py set-target`), delete it first so the defaults are
re-applied: `rm -f sdkconfig`.

## Selecting a board

The target board is chosen with the `BOARD` environment variable at configure
time (default: `tab5`). It selects the board source file, the component
manifest, and a board-specific sdkconfig layer.

```bash
idf.py build                     # default: tab5
BOARD=waveshare idf.py build
```

Switching boards changes the component set and sdkconfig — do a clean build when
you switch: `rm -rf build sdkconfig`.

## Build and flash

```bash
source ~/esp/esp-idf-v5.5.3/export.sh

idf.py set-target esp32p4              # only needed once
idf.py build                           # default tab5; BOARD=waveshare for the Waveshare
idf.py -p /dev/ttyACM0 flash monitor
```

No Wi-Fi credentials are compiled in — the device is provisioned at runtime
through the captive portal (see [First-boot Wi-Fi setup](README.md#first-boot-wi-fi-setup)).

> **Waveshare:** flash/monitor over the port labeled `UART`, not the OTG port.

## Seed the ESP32-C6 co-processor firmware (one-time)

The C6 runs `esp_hosted` slave firmware. Boards typically ship with v0.0.0
(works for Wi-Fi, no Bluetooth). The app OTAs the C6 to the bundled v2.12.3 on
boot — but only if the `slave_fw` partition contains the image. `idf.py flash`
does **not** populate that partition, so seed it once with esptool:

```bash
esptool.py --chip esp32p4 -p /dev/ttyACM0 write_flash --force \
    0x310000 slave_fw/network_adapter.bin
```

On the next boot, `slave_ota_update_if_needed()` (in `slave_ota.c`) reads that
partition, compares versions, and OTAs the C6 over SDIO if needed (~25 s, the C6
reboots once). It's idempotent — once current, later boots skip it. The host
`esp_hosted` is pinned to the matching 2.12.3 on Tab5; the Waveshare host is also
verified at 2.12.3.

The prebuilt release binaries skip this step by bundling the firmware directly
(see below), so end users never run esptool for the C6.

## Producing release binaries

`tools/build-release.sh` builds a single flashable image per board with the C6
firmware merged into the `slave_fw` partition, so the release artifact is
self-contained (flash one file to `0x0`, C6 auto-updates on first boot):

```bash
tools/build-release.sh             # all boards
tools/build-release.sh tab5        # one board
```

Output lands in `releases/p4-webradio-<board>-<version>.bin`. The merge is just
`esptool merge_bin @flash_args 0x310000 slave_fw/network_adapter.bin` per board
(plain `idf.py merge-bin` would omit the C6 firmware).

## Re-provisioning Wi-Fi

Saved Wi-Fi credentials and station settings (country, state, count) live in
NVS. To forget them and re-run first-boot setup:

```bash
idf.py -p /dev/ttyACM0 erase-flash
```

Note `erase-flash` also wipes the `slave_fw` partition — re-seed the C6 firmware
(above) after a full erase, or just reflash a bundled release binary.

## Architecture

Board-independent app + a per-board hardware abstraction (the `BOARD` variable
selects one `board_*.c`, à la esp32-gh-dashboard):

```
main.c          — app state machine, render loop, touch dispatch, NVS settings
board.h         — board interface: board_touch_init/read (+ display.h, audio.h)
board_waveshare_p4_720.c — Waveshare: ST7703 720×720, GT911, ES8311
board_m5stack_tab5.c     — Tab5: ST7123 720×1280 (landscape via PPA), ES8388, ST7123 touch
wifi.c          — ESP32-C6 hosted Wi-Fi bring-up via the wifi_prov component
slave_ota.c     — OTA-update the C6 firmware from the slave_fw partition
radio_browser.c — HTTPS fetch + JSON parse of the Radio Browser API (with retry)
stream.c        — HTTP audio streaming with ICY metadata into a ring buffer
mp3_decoder.c   — libhelix-mp3 decode task, feeds PCM to audio and the spectrum
spectrum.c      — 512-point FFT (esp-dsp), 32 log-spaced bands
touch.c         — board-independent gesture layer (tap thirds + swipes)
font8x16.c      — 8x16 VGA bitmap font, integer-scaled text overlay

components/esp_lcd_st7703                 — Waveshare panel driver (vendored)
components/esp_lcd_st7123                 — Tab5 panel driver (vendored)
components/wifi_prov                      — captive-portal Wi-Fi provisioning (SoftAP + web form, NVS)
components/nordesems__esp-captive-portal  — captive-portal DNS/HTTP redirect helper
```

`display.h`/`audio.h` are the rendering/audio APIs each board file implements;
`DISP_W`/`DISP_H` are runtime globals set by the active board.

### Notes

- **Display** uses hardware double-buffering (`num_fbs=2`, RGB565); `display_flush()`
  is cache-sync + `draw_bitmap` + buffer swap. On the Tab5 the panel is physically
  portrait, so the UI renders into a logical 1280×720 buffer and the **PPA**
  rotates it 90° into the framebuffer each flush.
- **Spectrum** bar width is derived from `DISP_W`, so the visualizer fills the
  whole screen on both the 720 and 1280 layouts.
- **Streaming** sends a VLC `User-Agent` (some stations require it). Stations that
  still refuse (HTTP 403 — geo-block/hotlink) show a "Currently Unavailable" screen.
- **ICY titles** are parsed by matching the closing `';` delimiter (apostrophes
  survive), a trailing URL is stripped, and text is transliterated to ASCII for
  the bitmap font.
- **Persistence**: current station (by URL) and volume are stored in NVS under the
  `radio` namespace and restored on boot.
- **Console**: UART primary + USB-JTAG secondary. USB-JTAG as the *primary* console
  is fragile on these boards (resets re-enumerate USB and logging stops).
