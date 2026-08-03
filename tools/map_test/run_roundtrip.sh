#!/bin/sh
# Mirrors the tile id <-> path logic in src/map_store.cpp and checks the property
# that matters: an id saved to disk comes back out of the scan as the SAME id,
# for every layout a card in the field might be in (prefix, flat, hash-sharded).
# Keep in step with tileDirFor/tilePathOf/saveTile if those change.
set -e
cd "$(dirname "$0")"
clang++ -std=c++17 -O2 -Wall roundtrip.cpp -o roundtrip
./roundtrip
