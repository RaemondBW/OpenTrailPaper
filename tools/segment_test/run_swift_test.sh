#!/bin/sh
# Builds and runs the iOS app's starred-segment parsing and device blob
# encoder on the Mac. See tools/segment_test/segments_test.swift.
set -eu
cd "$(dirname "$0")/../.."
build=$(mktemp -d /tmp/otp-segments-test.XXXXXX)
trap 'rm -rf "$build"' EXIT
xcrun swiftc -module-cache-path "$build/cache" companion-ios/Sources/CloudRoutes.swift \
    companion-ios/Sources/StravaSegments.swift tools/segment_test/segments_test.swift -o "$build/segments-test"
"$build/segments-test"
