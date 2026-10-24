#!/bin/sh
# Builds and runs the iOS app's GPX importer and Strava / RideWithGPS
# route-list parsers on the Mac. See tools/route_test/import_test.swift.
set -eu
cd "$(dirname "$0")/../.."
build=$(mktemp -d /tmp/otp-route-import-test.XXXXXX)
trap 'rm -rf "$build"' EXIT
xcrun swiftc -module-cache-path "$build/cache" companion-ios/Sources/GPXImporter.swift \
    companion-ios/Sources/CloudRoutes.swift tools/route_test/import_test.swift -o "$build/import-test"
"$build/import-test"
