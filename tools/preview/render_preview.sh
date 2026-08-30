#!/bin/sh
# Builds and runs the host screen-preview tool. Output: tools/preview/out/*.png
set -e
cd "$(dirname "$0")"

ROOT=../..
EPDIY=$ROOT/vendor/T5S3-4.7-e-paper-PRO/lib/epdiy/src

# The drawing functions come from src/epd_compat.cpp — the same rasteriser the
# firmware runs — not from epdiy's own epdiy.c/font.c as they used to. epdiy's
# headers are still needed for the types (EpdRect, EpdFont), hence -I "$EPDIY",
# but none of its source is compiled here any more, which also drops the four
# host shims (displays.c, builtin_waveforms.c, stubs.c) that only existed to make
# epdiy.c link on the host.
CXXFLAGS="-std=c++17 -O2 $EXTRA -I shim -I $EPDIY -I $ROOT/src"

for f in preview_main.cpp "$ROOT/src/epd_compat.cpp" "$ROOT/src/ui_render.cpp" "$ROOT/src/vfont.cpp" \
         "$ROOT/src/map_view.cpp" "$ROOT/src/map_tiles.cpp" \
         "$ROOT/src/dash_layout.cpp" "$ROOT/src/workout.cpp"; do
    clang++ $CXXFLAGS -c "$f" -o "/tmp/$(basename "$f" .cpp)_host.o"
done

clang++ /tmp/preview_main_host.o /tmp/epd_compat_host.o /tmp/ui_render_host.o /tmp/vfont_host.o \
    /tmp/map_view_host.o /tmp/map_tiles_host.o /tmp/dash_layout_host.o \
    /tmp/workout_host.o -lz -o preview

mkdir -p out
./preview out
