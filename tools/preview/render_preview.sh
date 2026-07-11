#!/bin/sh
# Builds and runs the host screen-preview tool. Output: tools/preview/out/*.png
set -e
cd "$(dirname "$0")"

ROOT=../..
EPDIY=$ROOT/vendor/T5S3-4.7-e-paper-PRO/lib/epdiy/src

clang -std=c11 -O2 -I shim -I "$EPDIY" -c "$EPDIY/epdiy.c" -o /tmp/epdiy_host.o
clang -std=c11 -O2 -I shim -I "$EPDIY" -c "$EPDIY/font.c" -o /tmp/font_host.o
clang -std=c11 -O2 -I shim -I "$EPDIY" -c "$EPDIY/displays.c" -o /tmp/displays_host.o
clang -std=c11 -O2 -I shim -I "$EPDIY" -c "$EPDIY/builtin_waveforms.c" -o /tmp/waveforms_host.o
clang -std=c11 -O2 -I shim -c stubs.c -o /tmp/stubs_host.o
clang++ -std=c++17 -O2 -I shim -I "$EPDIY" -I "$ROOT/src" \
    -c preview_main.cpp -o /tmp/preview_main.o
clang++ -std=c++17 -O2 -I shim -I "$EPDIY" -I "$ROOT/src" \
    -c "$ROOT/src/ui_render.cpp" -o /tmp/ui_render_host.o
clang++ -std=c++17 -O2 -I shim -I "$EPDIY" -I "$ROOT/src" \
    -c "$ROOT/src/map_view.cpp" -o /tmp/map_view_host.o
clang++ -std=c++17 -O2 -I shim -I "$EPDIY" -I "$ROOT/src" \
    -c "$ROOT/src/map_tiles.cpp" -o /tmp/map_tiles_host.o

clang++ /tmp/preview_main.o /tmp/ui_render_host.o /tmp/map_view_host.o /tmp/map_tiles_host.o \
    /tmp/epdiy_host.o /tmp/font_host.o /tmp/displays_host.o /tmp/waveforms_host.o /tmp/stubs_host.o \
    -lz -o preview

mkdir -p out
./preview out
