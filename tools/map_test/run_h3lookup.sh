#!/bin/sh
# Builds and runs the H3 tile-lookup check against a real tile directory laid
# out as on the SD card (<dir>/<first 6 of h3 id>/<rest>.ebm).
#
#   tools/map_test/run_h3lookup.sh <tiledir>
#
# This is the check that the index-free lookup rests on: the device computes the
# tile ids it needs from the rider's position and opens those files directly, so
# if ids or paths disagree with what the encoders wrote, it silently draws
# nothing. Uses the SAME vendored H3 the firmware and the app build.
set -e
cd "$(dirname "$0")"

ROOT=../..
H3=$ROOT/companion-ios/Sources/H3
OBJ=/tmp/h3lookup_obj
mkdir -p "$OBJ"

# H3 is C, not C++ — clang++ rejects its implicit void* conversions.
for f in "$H3"/h3shim.c "$H3"/lib/*.c; do
    clang -std=c11 -O2 -I "$H3" -I "$H3/include" -c "$f" \
        -o "$OBJ/$(basename "$f" .c).o"
done

clang++ -std=c++17 -O2 -I "$ROOT/src" -I "$H3" -I "$H3/include" \
    -c h3lookup.cpp -o "$OBJ/h3lookup.o"
clang++ "$OBJ"/*.o -lm -o h3lookup

./h3lookup "${1:?usage: run_h3lookup.sh <tiledir>}"
