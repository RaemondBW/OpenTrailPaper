#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
build=$(mktemp -d /tmp/otp-map-cache-test.XXXXXX)
trap 'rm -rf "$build"' EXIT
clang++ -std=c++17 -O1 -Wall -Wextra -fsanitize=address,undefined tools/map_cache_test/main.cpp -o "$build/test"
"$build/test"
