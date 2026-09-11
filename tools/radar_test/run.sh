#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
build=$(mktemp -d /tmp/otp-radar-test.XXXXXX)
trap 'rm -rf "$build"' EXIT
clang++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -I src src/radar.cpp src/dash_layout.cpp tools/radar_test/main.cpp -o "$build/test"
"$build/test"
if command -v xcrun >/dev/null 2>&1; then
    xcrun swiftc -module-cache-path "$build/cache" companion-ios/Sources/DashLayout.swift tools/radar_test/layout_test.swift -o "$build/swift-test"
    "$build/swift-test"
fi
