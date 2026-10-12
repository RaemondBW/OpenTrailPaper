#!/bin/sh
# Prove the server builder writes the same bytes as the iOS app, on the same
# OSM snapshot:
#   1. tools/tiles/build_region.mjs builds every cell of <box> from <pbf>.
#   2. dump_overpass.mjs writes, per cell, the Overpass responses a phone would
#      have received for a one-hex download — derived from the same <pbf>.
#   3. apptile/main.swift runs the app's own MapBuilder.swift / H3Tiles.swift
#      (host build, like tools/map_test/run_crossport.sh) on those responses.
#   4. compare.mjs: .ebm and .poi byte for byte, per cell.
#
#   tools/tiles/test/equivalence.sh <pbf> <s,w,n,e> [workdir]
# Needs node, osmium, swiftc + clang (Xcode).
set -e
cd "$(dirname "$0")/../../.."
PBF=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
BOX=$2
OUT=${3:-$(mktemp -d)}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
DEM=${DEM_CACHE:-$OUT/dem}
H3=companion-ios/Sources/H3

echo "== builder"
node tools/tiles/build_region.mjs --pbf "$PBF" --region equivalence --bbox "$BOX" \
    --out "$OUT/built" --tmp "$OUT/tmp" --dem-cache "$DEM"
echo "== Overpass responses from the same extract"
node tools/tiles/test/dump_overpass.mjs "$PBF" "$BOX" "$OUT/dump" --dem-cache "$DEM"

echo "== app code (MapBuilder.swift)"
mkdir -p "$OUT/obj"
for f in "$H3"/h3shim.c "$H3"/lib/*.c; do
    clang -std=c11 -O2 -I "$H3" -I "$H3/include" -c "$f" -o "$OUT/obj/$(basename "$f" .c).o"
done
swiftc -O -import-objc-header companion-ios/Sources/BikeGPS-Bridging-Header.h \
    -Xcc -I -Xcc "$H3" -Xcc -I -Xcc "$H3/include" \
    companion-ios/Sources/MapBuilder.swift companion-ios/Sources/H3Tiles.swift \
    tools/tiles/test/apptile/main.swift "$OUT"/obj/*.o -o "$OUT/apptile"
"$OUT/apptile" "$OUT/dump" "$OUT/app"

echo "== compare"
node tools/tiles/test/compare.mjs "$OUT/built" "$OUT/app" "$OUT/dump/tiles.txt"
