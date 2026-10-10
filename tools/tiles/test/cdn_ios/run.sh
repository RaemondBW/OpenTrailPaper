#!/bin/sh
# Host test of the iOS CDN tile source (companion-ios/Sources/PrebuiltTiles.swift).
#
#   tools/tiles/test/cdn_ios/run.sh <builder-out-tree> [workdir] [--time lat lon count]
#
# Serves <tree> (a build_region.mjs + merge_index.mjs output with v1/<g>/index.json)
# with python3 -m http.server, plus a tampered copy, and runs the app's own
# PrebuiltTiles.swift / MapBuilder.swift / H3Tiles.swift compiled for the host:
#   (a) indexed hexes come from the CDN byte-identical to the tree,
#   (b) hexes missing from an index, or whose group has no index, fall back,
#   (c) a truncated tile and a tile with a bad magic are rejected,
#   (d) an unreachable base URL falls back within seconds.
# With --time, also times the CDN path against the app's Overpass path
# (public Overpass + Open-Meteo, as a phone would) for the same hexes.
set -e
cd "$(dirname "$0")/../../../.."
TREE=$(cd "$1" && pwd)
WORK=${2:-$(mktemp -d)}
mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)
shift 2 || shift $#
H3=companion-ios/Sources/H3

mkdir -p "$WORK/obj"
for f in "$H3"/h3shim.c "$H3"/lib/*.c; do
    clang -std=c11 -O2 -I "$H3" -I "$H3/include" -c "$f" -o "$WORK/obj/$(basename "$f" .c).o"
done
swiftc -O -import-objc-header companion-ios/Sources/BikeGPS-Bridging-Header.h \
    -Xcc -I -Xcc "$H3" -Xcc -I -Xcc "$H3/include" \
    companion-ios/Sources/MapBuilder.swift companion-ios/Sources/H3Tiles.swift \
    companion-ios/Sources/PrebuiltTiles.swift \
    tools/tiles/test/cdn_ios/main.swift "$WORK"/obj/*.o -o "$WORK/cdn_ios"

# Tampered copy: one tile truncated, one with its magic overwritten.
rm -rf "$WORK/tampered"; cp -R "$TREE" "$WORK/tampered"
set -- $(ls "$WORK"/tampered/v1/*/*.ebm | head -2) "$@"
T1=$1; T2=$2; shift 2
head -c 1000 "$T1" > "$T1.tmp" && mv "$T1.tmp" "$T1"
printf 'XXXX' | dd of="$T2" bs=1 count=4 conv=notrunc 2>/dev/null
printf '%s\n%s\n' "$(basename "$T1" .ebm)" "$(basename "$T2" .ebm)" > "$WORK/tampered.txt"
# The checker reads tampered.txt from the tree it compares against; keep the
# original tree untouched by pointing it at a reference copy.
rm -rf "$WORK/ref"; cp -R "$TREE" "$WORK/ref"; cp "$WORK/tampered.txt" "$WORK/ref/"

P1=${PORT:-18631}; P2=$((P1 + 1))
python3 -m http.server "$P1" --bind 127.0.0.1 --directory "$WORK/ref" > "$WORK/http1.log" 2>&1 &
S1=$!
python3 -m http.server "$P2" --bind 127.0.0.1 --directory "$WORK/tampered" > "$WORK/http2.log" 2>&1 &
S2=$!
trap 'kill $S1 $S2 2>/dev/null' EXIT
sleep 1

"$WORK/cdn_ios" check "http://127.0.0.1:$P1/v1/" "$WORK/ref" "http://127.0.0.1:$P2/v1/"
if [ "$1" = "--time" ]; then
    "$WORK/cdn_ios" time "http://127.0.0.1:$P1/v1/" "$2" "$3" "$4"
fi
