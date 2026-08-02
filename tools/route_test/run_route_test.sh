#!/bin/sh
# Builds and runs the host route / turn-by-turn tests against the real
# src/routes.cpp. See tools/route_test/route_test.cpp for what is covered.
set -e
cd "$(dirname "$0")/../.."
clang++ -std=c++17 -O2 -Wall \
    -I tools/route_test/shim -I src \
    tools/route_test/route_test.cpp src/routes.cpp \
    -o tools/route_test/route_test
./tools/route_test/route_test
