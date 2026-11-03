#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
build=$(mktemp -d /tmp/otp-hybrid-test.XXXXXX)
trap 'rm -rf "$build"' EXIT
clang++ -std=c++17 -O2 -Wall -Wextra -fsanitize=address,undefined tools/epd_hybrid_bench/core_test.cpp -o "$build/test"
"$build/test"
