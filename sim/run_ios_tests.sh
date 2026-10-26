#!/bin/sh
# Runs the iOS app's firmware-sim UI tests (companion-ios/UITests/
# FirmwareSimTests.swift, sim/IOS.md): builds the sim if needed, starts a
# factory-fresh otp-sim, runs the tests in the iOS Simulator with the app on
# SimRadio, then stops the sim.
#
#   sim/run_ios_tests.sh                     # all FirmwareSimTests
#   sim/run_ios_tests.sh test03_Background…  # one test (method name)
#
# Env: OTP_IOS_SIM (simulator UDID), OTP_SIM_BLE_PORT / OTP_SIM_CTL_PORT /
# OTP_SIM_RADIO_PORT (default 7920 / 7921 / 7922), OTP_DERIVED_DATA.
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)

UDID=${OTP_IOS_SIM:-14EF0FB8-65FC-468C-B51C-F62B9D2C4369}
BLE=${OTP_SIM_BLE_PORT:-7920}
CTL=${OTP_SIM_CTL_PORT:-7921}
RADIO=${OTP_SIM_RADIO_PORT:-7922}
BUNDLE=com.raemond.opentrailpaper
DD=${OTP_DERIVED_DATA:-$ROOT/sim/build/ios-dd}
ONLY=OpenTrailPaperUITests/FirmwareSimTests
[ $# -gt 0 ] && ONLY="$ONLY/$1"

# 1. The sim, rebuilt when the firmware or the sim's sources are newer.
BIN=sim/build/otp-sim
if [ ! -x "$BIN" ] || [ -n "$(find src sim/host sim/shim -newer "$BIN" -type f 2>/dev/null | head -1)" ]; then
    echo "== building the firmware sim"
    sim/build.sh
fi

for p in "$BLE" "$CTL" "$RADIO"; do
    if lsof -nP -iTCP:"$p" -sTCP:LISTEN >/dev/null 2>&1; then
        echo "port $p is in use (another otp-sim?); set OTP_SIM_*_PORT" >&2
        exit 1
    fi
done

# 2. A factory-fresh device.
DIR=$(mktemp -d "${TMPDIR:-/tmp}/otpsim-ios.XXXXXX")
LOG="$DIR/otp-sim.log"
"$BIN" --dir "$DIR/device" --ble-port "$BLE" --ctl-port "$CTL" --no-stdin --quiet >"$LOG" 2>&1 &
SIM_PID=$!
cleanup() {
    kill "$SIM_PID" 2>/dev/null || true
    wait "$SIM_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
i=0
until printf '{"cmd":"status"}\n' | nc -w 1 127.0.0.1 "$CTL" 2>/dev/null | grep -q '"ok":true'; do
    i=$((i + 1))
    if [ $i -gt 50 ] || ! kill -0 "$SIM_PID" 2>/dev/null; then
        echo "otp-sim did not come up; log: $LOG" >&2
        cat "$LOG" >&2
        exit 1
    fi
    sleep 0.2
done
echo "== otp-sim pid $SIM_PID: simble $BLE, control $CTL, state $DIR"

# 3. A fresh app (no remembered device, no bond) on a booted simulator.
xcrun simctl boot "$UDID" 2>/dev/null || true
xcrun simctl bootstatus "$UDID" -b >/dev/null
xcrun simctl terminate "$UDID" "$BUNDLE" 2>/dev/null || true
xcrun simctl uninstall "$UDID" "$BUNDLE" 2>/dev/null || true
# The app streams the phone's position once connected; no prompt mid-test.
xcrun simctl privacy "$UDID" grant location "$BUNDLE" 2>/dev/null || true

# The app's log ([ble], [link], [sim-ble] lines), for reading after a run.
xcrun simctl spawn "$UDID" log stream --style compact \
    --predicate 'process == "OpenTrailPaper" AND (eventMessage CONTAINS "[ble]" OR eventMessage CONTAINS "[link]" OR eventMessage CONTAINS "[sim-ble]")' \
    >"$DIR/app.log" 2>&1 &
LOG_PID=$!
cleanup() {
    kill "$SIM_PID" "$LOG_PID" 2>/dev/null || true
    wait "$SIM_PID" 2>/dev/null || true
}

# 4. The tests. TEST_RUNNER_* reaches the test runner's environment.
set +e
TEST_RUNNER_OTP_SIM_BLE_PORT=$BLE \
TEST_RUNNER_OTP_SIM_CTL_PORT=$CTL \
TEST_RUNNER_OTP_SIM_RADIO_PORT=$RADIO \
xcodebuild test \
    -project companion-ios/OpenTrailPaper.xcodeproj \
    -scheme OpenTrailPaper \
    -destination "id=$UDID" \
    -derivedDataPath "$DD" \
    -only-testing:"$ONLY" \
    -resultBundlePath "$DIR/results.xcresult" \
    2>&1 | tee "$DIR/xcodebuild.log" | grep -E '^(Test Case|Test Suite|\*\*|.*error:|.*: error|.*XCTAssert|    Executed)'
RC=$(grep -q '\*\* TEST SUCCEEDED \*\*' "$DIR/xcodebuild.log" && echo 0 || echo 1)
set -e
echo "== logs: $DIR (otp-sim.log, app.log, xcodebuild.log, results.xcresult)"
exit "$RC"
