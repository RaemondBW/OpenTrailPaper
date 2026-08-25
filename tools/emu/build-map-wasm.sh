#!/bin/sh
# Compile the firmware's OWN map renderer (map_tiles + ui_render + the
# epd_compat rasteriser) to WebAssembly, with the San Francisco map embedded, so
# the web emulator can draw the map page in the browser. QEMU can't render it —
# its esp32s3 PSRAM model is broken and the projector needs PSRAM scratch — but
# the same code runs fine with the browser's unbounded RAM. See web/emulator/
# map_wasm.cpp. Output: web/emulator/map_wasm.js (self-contained, wasm inlined).
#
#   source <emsdk>/emsdk_env.sh   # emcc on PATH
#   tools/emu/build-map-wasm.sh
set -e
cd "$(dirname "$0")/../.."

EMSDK_ENV="${EMSDK_ENV:-$HOME/emsdk/emsdk_env.sh}"
if ! command -v em++ >/dev/null 2>&1; then
    [ -f "$EMSDK_ENV" ] && . "$EMSDK_ENV" || {
        echo "emcc not found. Install emsdk and 'source emsdk_env.sh', or set EMSDK_ENV." >&2
        exit 1
    }
fi

EPDIY=vendor/T5S3-4.7-e-paper-PRO/lib/epdiy/src
SHIM=tools/preview/shim   # host shims for the ESP-IDF headers these sources pull

# The SAME rendering translation units the host preview links (render_preview.sh)
# — none of them touch Arduino/PSRAM, which is why they build off-target.
em++ -O2 -std=c++17 \
    -I "$SHIM" -I "$EPDIY" -I src \
    web/emulator/map_wasm.cpp \
    src/epd_compat.cpp \
    src/ui_render.cpp \
    src/map_view.cpp \
    src/map_tiles.cpp \
    src/dash_layout.cpp \
    src/workout.cpp \
    --embed-file data/sf.ebm@sf.ebm \
    -s MODULARIZE=1 -s EXPORT_NAME=MapModule \
    -s SINGLE_FILE=1 \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s ENVIRONMENT=web \
    -s EXPORTED_FUNCTIONS='["_map_init","_map_fb","_map_fb_len","_map_render","_malloc","_free"]' \
    -s EXPORTED_RUNTIME_METHODS='["cwrap","ccall","HEAPU8"]' \
    -o web/emulator/map_wasm.js

echo "wrote web/emulator/map_wasm.js ($(du -h web/emulator/map_wasm.js | cut -f1))"
