#!/bin/sh
# Builds and runs the host tests for the phone-stream speed/heading estimator
# (src/phone_motion.h). See tools/phone_motion_test/phone_motion_test.cpp.
set -e
cd "$(dirname "$0")/../.."
clang++ -std=c++17 -O2 -Wall -I src \
    tools/phone_motion_test/phone_motion_test.cpp -o tools/phone_motion_test/phone_motion_test
./tools/phone_motion_test/phone_motion_test
