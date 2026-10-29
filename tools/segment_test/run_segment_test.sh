#!/bin/sh
# Builds and runs the host tests for live Strava segments (src/segment_match.h):
# the device blob format and the matcher on synthetic rides. See
# tools/segment_test/segment_test.cpp for what is covered.
set -e
cd "$(dirname "$0")/../.."
clang++ -std=c++17 -O2 -Wall -I src \
    tools/segment_test/segment_test.cpp -o tools/segment_test/segment_test
./tools/segment_test/segment_test
