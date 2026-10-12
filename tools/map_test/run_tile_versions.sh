#!/bin/sh
# Builds and runs the host tests for src/tile_versions.h, the on-card record of
# which version each tile and .poi was saved with. See tile_versions_test.cpp.
set -e
cd "$(dirname "$0")/../.."
OUT=${TMPDIR:-/tmp}/tile_versions_test
clang++ -std=c++17 -O2 -Wall -I src tools/map_test/tile_versions_test.cpp -o "$OUT"
"$OUT"
