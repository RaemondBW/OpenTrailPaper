#!/bin/sh
# Builds and runs the host Meshtastic wire-format tests against the real
# src/mesh_proto.cpp. See tools/mesh_test/mesh_test.cpp for what is covered.
#
# Worth running after ANY edit to mesh_proto.cpp: the values it checks (channel
# frequency slot, channel hash, AES keystream, header byte layout) are what make
# the device audible to other Meshtastic nodes, and every one of them fails
# silently on real hardware.
set -e
cd "$(dirname "$0")/../.."

# clang++ locally, g++ on a Linux CI runner that may not have it. mesh_proto.cpp
# is plain C++ with no Arduino or ESP-IDF dependency, so either will do.
CXX=${CXX:-}
if [ -z "$CXX" ]; then
    if command -v clang++ >/dev/null 2>&1; then CXX=clang++; else CXX=g++; fi
fi

"$CXX" -std=c++17 -O2 -Wall -Wextra \
    -I src \
    tools/mesh_test/mesh_test.cpp src/mesh_proto.cpp \
    -o tools/mesh_test/mesh_test
./tools/mesh_test/mesh_test
