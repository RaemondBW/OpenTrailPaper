#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
build=$(mktemp -d /tmp/otp-scan-test.XXXXXX)
trap 'rm -rf "$build"' EXIT
clang++ -std=c++17 -O2 -Wall -Wextra -fsanitize=address,undefined tools/scan_test/main.cpp -o "$build/test"
"$build/test"
