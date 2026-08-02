#!/usr/bin/env bash
# One-shot revert of the PM (light-sleep) framework experiment.
# See REVERT-PM.md. Safe to run at any time, including when nothing is broken.

set -uo pipefail

PKGS="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages"
FW="$PKGS/framework-arduinoespressif32"
STOCK="$FW.stock"
PROJ="$(cd "$(dirname "$0")/.." && pwd)"

echo "Reverting the PM framework experiment."
echo "  framework: $FW"
echo "  project:   $PROJ"
echo

if [ -d "$STOCK" ]; then
    echo "==> restoring from local stock backup ($STOCK)"
    rm -rf "$FW"
    cp -a "$STOCK" "$FW" || { echo "FAIL: copy failed"; exit 1; }
else
    echo "==> no local backup; deleting and re-downloading (a few minutes)"
    rm -rf "$FW"
fi

echo "==> clearing build cache and libdeps ($PROJ/.pio)"
rm -rf "$PROJ/.pio"

if [ ! -d "$FW" ]; then
    echo "==> pio pkg install"
    ( cd "$PROJ" && pio pkg install -e t5s3-painter ) || { echo "FAIL: install failed"; exit 1; }
fi

echo
echo "==> verifying"
"$PROJ/tools/check-framework.sh" || {
    echo
    echo "Framework still does not match. Try option 2 or 3 in REVERT-PM.md."
    exit 1
}

echo
echo "==> test build"
( cd "$PROJ" && pio run -e t5s3-painter ) || { echo "FAIL: build failed"; exit 1; }

echo
echo "Reverted. Flash with:  pio run -e t5s3-painter -t upload --upload-port /dev/cu.usbmodem2101"
