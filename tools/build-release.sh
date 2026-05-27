#!/usr/bin/env bash
# Build flashable release binaries for every supported board.
#
# Each output is a single image, flashable at 0x0, that bundles the ESP32-C6
# co-processor firmware in the slave_fw partition (0x310000). On first boot the
# P4 OTAs the C6 from there automatically, so end users flash one file and need
# to know nothing about the co-processor. See slave_ota.c.
#
# Usage:  tools/build-release.sh [board ...]      (default: all boards)
# Output: releases/p4-webradio-<board>-<version>.bin
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
OUT="$ROOT/releases"
SLAVE_FW="$ROOT/slave_fw/network_adapter.bin"
DEFAULT_BOARDS=(tab5 waveshare)
BOARDS=("${@:-${DEFAULT_BOARDS[@]}}")
VERSION="$(git describe --tags --always --dirty 2>/dev/null || echo dev)"

[ -f "$SLAVE_FW" ] || { echo "ERROR: missing $SLAVE_FW"; exit 1; }

# Source ESP-IDF (same version logic as ~/bin/idf) so idf.py + esptool.py are
# on PATH. Skip if the environment is already set up.
if ! command -v esptool.py >/dev/null 2>&1; then
    IDF_VERSION="$(tr -d '[:space:]' < .idf-version 2>/dev/null || true)"
    IDF_VERSION="${IDF_VERSION:-5.5.3}"
    for cand in "$HOME/esp/esp-idf-v${IDF_VERSION}/export.sh" \
                "$HOME/.espressif/v${IDF_VERSION}/esp-idf/export.sh" \
                "$HOME/.espressif/${IDF_VERSION}/esp-idf/export.sh"; do
        if [ -f "$cand" ]; then . "$cand"; break; fi
    done
fi
command -v esptool.py >/dev/null 2>&1 || { echo "ERROR: ESP-IDF not found; source export.sh first"; exit 1; }

mkdir -p "$OUT"

for BOARD in "${BOARDS[@]}"; do
    echo "=============================="
    echo " Building board: $BOARD ($VERSION)"
    echo "=============================="
    # Fully clean between boards: each board pulls a different component set and
    # sdkconfig layer, so a stale build dir can't be trusted for a release.
    rm -rf "$ROOT/build" "$ROOT/sdkconfig"
    BOARD="$BOARD" idf.py build

    # Merge bootloader + partition table + app + C6 firmware into one image.
    ( cd "$ROOT/build" && esptool.py --chip esp32p4 merge_bin \
        -o "$OUT/p4-webradio-${BOARD}-${VERSION}.bin" \
        @flash_args 0x310000 "$SLAVE_FW" )
done

echo
echo "Release binaries (flash to 0x0):"
ls -la "$OUT"/p4-webradio-*-"$VERSION".bin
