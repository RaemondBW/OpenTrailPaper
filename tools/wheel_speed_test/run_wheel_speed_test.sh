#!/bin/sh
# Builds and runs the host tests for the CSC wheel-speed maths
# (src/wheel_speed.h) and the speed-source arbitration (src/speed_source.h).
set -e
cd "$(dirname "$0")/../.."
CXX="${CXX:-c++}"
"$CXX" -std=c++17 -O2 -Wall -I src \
    tools/wheel_speed_test/wheel_speed_test.cpp -o tools/wheel_speed_test/wheel_speed_test
./tools/wheel_speed_test/wheel_speed_test
