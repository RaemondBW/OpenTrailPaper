#!/bin/sh
# Builds the firmware sim if needed and runs its test suite (sim/tests).
# Arguments go to pytest: sim/test.sh -k pairing -x
set -e
cd "$(dirname "$0")"
./build.sh > /dev/null
if [ ! -x .venv/bin/python ]; then
    python3 -m venv .venv
    .venv/bin/pip install -q "protobuf>=5,<7" pytest
fi
cd tests
exec ../.venv/bin/python -m pytest -p no:cacheprovider "$@"
