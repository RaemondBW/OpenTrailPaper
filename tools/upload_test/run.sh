#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
build=$(mktemp -d /tmp/otp-upload-test.XXXXXX)
trap 'rm -rf "$build"' EXIT
xcrun swiftc -module-cache-path "$build/cache" companion-ios/Sources/RideUploadService.swift tools/upload_test/main.swift -o "$build/test"
"$build/test"
