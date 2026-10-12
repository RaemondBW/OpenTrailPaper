#!/bin/sh
# Compile the iOS app's pure settings-sync model with a test driver and run it.
# (The iOS app has no XCTest target; the Android twin of these cases runs as
# the JUnit test DeviceSettingsSyncTest.)
set -e
here="$(cd "$(dirname "$0")" && pwd)"
out="${TMPDIR:-/tmp}/offline_settings_test"
xcrun swiftc "$here/../../companion-ios/Sources/DeviceSettingsSync.swift" "$here/main.swift" -o "$out"
"$out"
