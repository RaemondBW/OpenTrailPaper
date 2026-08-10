#!/bin/sh
# Builds and runs the host tests for the optional Qwiic sensor maths
# (src/aux_math.h). See tools/aux_test/aux_test.cpp for what is covered.
set -e
cd "$(dirname "$0")/../.."
clang++ -std=c++17 -O2 -Wall -I src \
    tools/aux_test/aux_test.cpp -o tools/aux_test/aux_test
./tools/aux_test/aux_test
