#!/bin/sh
# Builds and runs the multi-tile map scene renderer. Unlike tools/preview (one
# .ebm blob) this projects a directory of real H3 res-6 tiles through the same
# code path map_store::renderInto drives on the device, so the per-frame scratch
# budgets and the tile cap are actually exercised.
#
#   tools/map_test/run_tilescene.sh <tiledir> [outdir] [lat lon]
#
# <tiledir> is laid out like the SD card: <tiledir>/<first 6 of h3 id>/<rest>.ebm
# — i.e. exactly what the website's map generator produces, unzipped.
# Output: <outdir>/map_{northup,trackup}_mpp*.png (default tools/map_test/out).
set -e
cd "$(dirname "$0")"

TILES=${1:?usage: run_tilescene.sh <tiledir> [outdir] [lat lon]}
OUT=${2:-out}
shift 2 2>/dev/null || shift $#

ROOT=../..
EPDIY=$ROOT/vendor/T5S3-4.7-e-paper-PRO/lib/epdiy/src
CXXFLAGS="-std=c++17 -O2 -I ../preview/shim -I $EPDIY -I $ROOT/src"

for f in tilescene.cpp "$ROOT/src/epd_compat.cpp" "$ROOT/src/ui_render.cpp" \
         "$ROOT/src/map_view.cpp" "$ROOT/src/map_tiles.cpp" \
         "$ROOT/src/dash_layout.cpp"; do
    clang++ $CXXFLAGS -c "$f" -o "/tmp/$(basename "$f" .cpp)_tile.o"
done
clang++ /tmp/tilescene_tile.o /tmp/epd_compat_tile.o /tmp/ui_render_tile.o \
    /tmp/map_view_tile.o /tmp/map_tiles_tile.o /tmp/dash_layout_tile.o \
    -lz -o tilescene

mkdir -p "$OUT"
./tilescene "$TILES" "$OUT" "$@"
