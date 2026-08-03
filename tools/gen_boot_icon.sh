#!/bin/sh
# Regenerate src/boot_icon.h from the iOS app icon, so the device boot screen
# and the phone app show the same mark.
#
# Uses sips (macOS) to resize + convert to BMP, then packs to the 4bpp format
# epd_draw_pixel uses. No Python imaging dependency.
#
#   sh tools/gen_boot_icon.sh [size]
set -e
cd "$(dirname "$0")/.."
SIZE="${1:-220}"
SRC=companion-ios/Sources/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png
TMP=$(mktemp -d)
sips -z "$SIZE" "$SIZE" -s format bmp "$SRC" --out "$TMP/icon.bmp" >/dev/null
python3 tools/gen_boot_icon.py "$TMP/icon.bmp" src/boot_icon.h
rm -rf "$TMP"
echo "wrote src/boot_icon.h (${SIZE}x${SIZE})"
