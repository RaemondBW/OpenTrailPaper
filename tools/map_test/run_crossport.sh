#!/bin/sh
# Cross-port check: one saved Overpass response through the three map builders
# — docs/mapgen.js (website, the reference), the iOS app's MapBuilder.swift and
# the Android app's MapBuilder.kt — comparing, per H3 tile, the .ebm road data
# (with the cycling way-flag trailers) and the .poi files byte for byte.
#
#   tools/map_test/run_crossport.sh <overpass.json> <s> <w> <n> <e> [outdir]
#
# <overpass.json> is a raw response to the website's combined query (mapgen.js
# fetchOverpass), or any response containing the elements to compare. The box
# picks the tiles. Needs node, swiftc + clang (Xcode). The Kotlin side runs as a
# JVM unit test when ANDROID_CROSSPORT=1 (needs the Android build env: JAVA_HOME
# and an SDK in companion-android/local.properties). H3-cell .poi files are
# compared when H3JS_DIR points at a directory with node_modules/h3-js; without
# it the bbox variant is compared on every port.
set -e
cd "$(dirname "$0")/../.."
JSON=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
S=$2; W=$3; N=$4; E=$5
OUT=${6:-$(mktemp -d)}
OUT=$(mkdir -p "$OUT" && cd "$OUT" && pwd)
H3=companion-ios/Sources/H3

# --- Swift driver (host build of the app's MapBuilder + the vendored H3 C)
mkdir -p "$OUT/obj"
for f in "$H3"/h3shim.c "$H3"/lib/*.c; do
    clang -std=c11 -O2 -I "$H3" -I "$H3/include" -c "$f" -o "$OUT/obj/$(basename "$f" .c).o"
done
swiftc -O -import-objc-header companion-ios/Sources/BikeGPS-Bridging-Header.h \
    -Xcc -I -Xcc "$H3" -Xcc -I -Xcc "$H3/include" \
    companion-ios/Sources/MapBuilder.swift companion-ios/Sources/H3Tiles.swift \
    tools/map_test/crossport/main.swift "$OUT"/obj/*.o -o "$OUT/crossport_swift"

"$OUT/crossport_swift" tiles "$S" "$W" "$N" "$E" > "$OUT/tiles.txt"
echo "$(wc -l < "$OUT/tiles.txt" | tr -d ' ') tiles"

node tools/map_test/crossport/crossport.mjs "$JSON" "$OUT/tiles.txt" "$OUT/js"
"$OUT/crossport_swift" "$JSON" "$OUT/tiles.txt" "$OUT/swift"
ports="js swift"
if [ "${ANDROID_CROSSPORT:-0}" = 1 ]; then
    (cd companion-android && CROSSPORT_JSON="$JSON" CROSSPORT_TILES="$OUT/tiles.txt" \
        CROSSPORT_OUT="$OUT/kotlin" ./gradlew -q testDebugUnitTest \
        --tests 'com.raemond.opentrailpaper.CrossPortTest' --rerun-tasks)
    ports="js swift kotlin"
fi

fail=0
for kind in ebm bbox.poi poi; do
    for f in "$OUT"/swift/*."$kind"; do
        b=$(basename "$f")
        case "$kind:$b" in poi:*.bbox.poi) continue ;; esac
        for p in $ports; do
            [ "$p" = swift ] && continue
            ref="$OUT/$p/$b"
            if [ ! -f "$ref" ]; then
                [ "$kind" = poi ] && continue   # no H3 on that port: bbox only
                echo "MISSING $p/$b"; fail=1; continue
            fi
            if cmp -s "$f" "$ref"; then :; else echo "DIFF swift vs $p: $b"; fail=1; fi
        done
    done
    echo "checked .$kind"
done
[ $fail -eq 0 ] && echo "cross-port: all match ($ports)" || { echo "cross-port: MISMATCH"; exit 1; }
