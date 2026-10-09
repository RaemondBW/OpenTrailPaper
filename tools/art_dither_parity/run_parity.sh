#!/bin/sh
# Byte-for-byte parity between the iOS app's album-art dither
# (companion-ios/Sources/ArtDither.swift) and the firmware's (src/media.cpp
# commitArt, compiled for the host). Also round-trips the app's base-5 packing
# through the firmware's tone-art unpack. Run from anywhere; needs clang++ and
# swiftc (Xcode).
set -e
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
clang++ -std=c++17 -O2 -w -I tools/art_dither_parity/shim -I src \
    src/media.cpp tools/art_dither_parity/shim/diag_stub.cpp \
    tools/art_dither_parity/firmware_dither.cpp -o "$out/fw"
swiftc -O companion-ios/Sources/ArtDither.swift \
    tools/art_dither_parity/swift/main.swift -o "$out/sw"
fail=0
for size in "300 300" "320 320" "37 23" "1 1" "320 1" "2 5"; do
    for recipe in 0 1 2 3 4; do
        set -- $size
        "$out/fw" "$1" "$2" "$recipe" > "$out/fw.txt"
        "$out/sw" "$1" "$2" "$recipe" > "$out/sw.txt"
        "$out/sw" "$1" "$2" "$recipe" pack | "$out/fw" unpack "$1" "$2" > "$out/rt.txt"
        if cmp -s "$out/fw.txt" "$out/sw.txt" && cmp -s "$out/fw.txt" "$out/rt.txt"; then
            echo "ok   ${1}x${2} recipe $recipe"
        else
            echo "FAIL ${1}x${2} recipe $recipe"; fail=1
        fi
    done
done
[ $fail -eq 0 ] && echo "art dither parity: all match" || { echo "art dither parity: MISMATCH"; exit 1; }
