#!/bin/sh
# Builds and runs the host FitWriter test, then validates the emitted .fit
# files with a real FIT parser if one is installed (pip install fitdecode).
# Output: tools/fit_test/out/*.fit
set -e
cd "$(dirname "$0")/../.."

mkdir -p tools/fit_test/out
clang++ -std=c++17 -O2 -Wall \
    -I tools/fit_test/shim -I src \
    tools/fit_test/fit_test.cpp src/fit_writer.cpp \
    -o tools/fit_test/fit_test

./tools/fit_test/fit_test

# empty.fit is deliberately unrecoverable (the device deletes it rather than
# keeping a ride with no points), so it is not expected to parse.
if python3 -c "import fitdecode" 2>/dev/null; then
    echo
    python3 tools/fit_test/validate_fit.py \
        tools/fit_test/out/normal.fit \
        tools/fit_test/out/crashed.fit \
        tools/fit_test/out/torn.fit
else
    echo
    echo "(skipping parser validation — pip install fitdecode to enable)"
fi
