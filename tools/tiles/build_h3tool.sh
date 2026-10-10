#!/bin/sh
# Build tools/tiles/h3tool from the H3 C sources the iOS/Android apps vendor
# (companion-ios/Sources/H3), so the server uses the apps' exact H3 math.
#   tools/tiles/build_h3tool.sh [outfile]   (default: tools/tiles/.bin/h3tool)
set -e
here=$(cd "$(dirname "$0")" && pwd)
H3="$here/../../companion-ios/Sources/H3"
out=${1:-$here/.bin/h3tool}
mkdir -p "$(dirname "$out")"
${CC:-cc} -O2 -std=c11 -I "$H3" -I "$H3/include" \
    "$here/h3tool.c" "$H3/h3shim.c" "$H3"/lib/*.c -lm -o "$out"
echo "$out"
