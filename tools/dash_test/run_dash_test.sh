#!/bin/sh
# Builds and runs the host tests for the dashboard layout system
# (src/dash_layout.h). See tools/dash_test/dash_test.cpp for what is covered.
set -e
cd "$(dirname "$0")/../.."
clang++ -std=c++17 -O2 -Wall -I src \
    tools/dash_test/dash_test.cpp src/dash_layout.cpp -o tools/dash_test/dash_test
./tools/dash_test/dash_test
