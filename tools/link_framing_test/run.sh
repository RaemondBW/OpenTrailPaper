#!/bin/sh
# Compile the iOS app's link framing + CRC-32 (LinkFraming.swift) with a test
# driver and run it. (The iOS app has no XCTest target.)
set -e
here="$(cd "$(dirname "$0")" && pwd)"
out="${TMPDIR:-/tmp}/link_framing_test"
xcrun swiftc "$here/../../companion-ios/Sources/LinkFraming.swift" "$here/main.swift" -o "$out"
"$out"
