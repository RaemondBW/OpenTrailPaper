#!/bin/sh
# Builds and runs the vector-font specimen on the host. Output: tools/vfont/out/*.png
set -e
cd "$(dirname "$0")"
ROOT=../..
EPDIY=$ROOT/vendor/T5S3-4.7-e-paper-PRO/lib/epdiy/src
PREVIEW=$ROOT/tools/preview
CXXFLAGS="-std=c++17 -O2 -I $PREVIEW/shim -I $EPDIY -I $ROOT/src"
mkdir -p out
for f in specimen.cpp "$ROOT/src/epd_compat.cpp" "$ROOT/src/ui_render.cpp" "$ROOT/src/vfont.cpp" \
         "$ROOT/src/map_view.cpp" "$ROOT/src/map_tiles.cpp" "$ROOT/src/dash_layout.cpp" "$ROOT/src/workout.cpp"; do
    clang++ $CXXFLAGS -c "$f" -o "out/$(basename "$f" .cpp).o"
done
clang++ out/*.o -lz -o out/specimen
./out/specimen out
PY=$ROOT/.venv-font/bin/python
[ -x "$PY" ] || PY=python3
$PY -c "
from PIL import Image; import glob
for p in glob.glob('out/*.pgm'): Image.open(p).save(p[:-4]+'.png')
"
ls out/*.png
