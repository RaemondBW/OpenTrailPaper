#!/bin/sh
# Runs the iOS app's firmware-sim UI tests (companion-ios/UITests/
# FirmwareSimTests.swift and FirmwareSimAppFlowTests.swift, sim/IOS.md):
# builds the sim if needed and, for each test class, starts a factory-fresh
# otp-sim and the stand-in tile CDN (sim/cdn/tile_cdn.py), runs the class in
# the iOS Simulator with a freshly installed app on SimRadio, then stops both.
# Each class gets a device of its own (each pairs its phone first).
#
#   sim/run_ios_tests.sh                          # every class
#   sim/run_ios_tests.sh FirmwareSimAppFlowTests  # one class
#   sim/run_ios_tests.sh test03_Background…       # one test (method name)
#   sim/run_ios_tests.sh FirmwareSimTests/test03… # one test, class given
#
# Env: OTP_IOS_SIM (simulator UDID), OTP_SIM_BLE_PORT / OTP_SIM_CTL_PORT /
# OTP_SIM_RADIO_PORT / OTP_CDN_PORT (default 7920 / 7921 / 7922 / 7923),
# OTP_CDN_FIXTURE (the CDN's directory, default sim/fixtures/appmaps),
# OTP_DERIVED_DATA,
# OTP_XCODEBUILD_ARGS (extra xcodebuild settings, e.g. CODE_SIGNING_ALLOWED=NO
# on a runner with no signing certificate).
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)

UDID=${OTP_IOS_SIM:-14EF0FB8-65FC-468C-B51C-F62B9D2C4369}
# No such simulator here (a CI runner): use the first available iPhone.
if ! xcrun simctl list devices available | grep -q "$UDID"; then
    UDID=$(xcrun simctl list devices available -j | python3 -c '
import json, sys
d = json.load(sys.stdin)["devices"]
ios = sorted((r for r in d if "iOS" in r), key=lambda r: [int(x) for x in r.rsplit("iOS-", 1)[1].split("-")], reverse=True)
print(next(dev["udid"] for r in ios for dev in d[r] if dev["name"].startswith("iPhone")))')
    echo "== simulator $UDID"
fi
BLE=${OTP_SIM_BLE_PORT:-7920}
CTL=${OTP_SIM_CTL_PORT:-7921}
RADIO=${OTP_SIM_RADIO_PORT:-7922}
CDN=${OTP_CDN_PORT:-7923}
FIXTURE=${OTP_CDN_FIXTURE:-sim/fixtures/appmaps}
case $FIXTURE in /*) ;; *) FIXTURE=$ROOT/$FIXTURE ;; esac
BUNDLE=com.raemond.opentrailpaper
DD=${OTP_DERIVED_DATA:-$ROOT/sim/build/ios-dd}
CLASSES="FirmwareSimTests FirmwareSimAppFlowTests"

# What to run: each argument is a class, "Class/test" or a test's name; each
# gets its own factory-fresh device. No arguments: every class.
RUNS=$CLASSES
if [ $# -gt 0 ]; then
    RUNS=
    for a in "$@"; do
        case $a in
            */*) RUNS="$RUNS $a" ;;
            *)
                if echo " $CLASSES " | grep -q " $a "; then
                    RUNS="$RUNS $a"
                else
                    FILE=$(grep -l "func $a(" companion-ios/UITests/*.swift | head -1)
                    [ -n "$FILE" ] || { echo "no test $a in companion-ios/UITests" >&2; exit 1; }
                    RUNS="$RUNS $(basename "$FILE" .swift)/$a"
                fi
                ;;
        esac
    done
fi

# 1. The sim, rebuilt when the firmware or the sim's sources are newer.
BIN=sim/build/otp-sim
if [ ! -x "$BIN" ] || [ -n "$(find src sim/host sim/shim -newer "$BIN" -type f 2>/dev/null | head -1)" ]; then
    echo "== building the firmware sim"
    sim/build.sh
fi

for p in "$BLE" "$CTL" "$RADIO" "$CDN"; do
    if lsof -nP -iTCP:"$p" -sTCP:LISTEN >/dev/null 2>&1; then
        echo "port $p is in use (another otp-sim?); set OTP_SIM_*_PORT" >&2
        exit 1
    fi
done

TOP=$(mktemp -d "${TMPDIR:-/tmp}/otpsim-ios.XXXXXX")

# Rides for FirmwareSimAppFlowTests, written by the firmware's own FitWriter
# (otp-sim card-tool fit): "summary" (the newest), "long" (four hours) and
# forty short "history" rides a day apart. manifest.txt names them by role.
RIDES=$TOP/ride-fixtures
mkdir -p "$RIDES"
python3 - "$RIDES/specs.txt" "$RIDES/manifest.txt" <<'EOF'
import sys, time
now = int(time.time()) // 3600 * 3600
def name(t): return time.strftime("%Y%m%d-%H%M%S", time.gmtime(t)) + ".fit"
rides = [("summary", now - 86400 + 7 * 60, 3600, 27.0, 145, 210),
         ("long", now - 50 * 86400 + 9 * 60, 4 * 3600, 24.0, 140, 190)]
rides += [("history-%02d" % i, now - (i + 2) * 86400 + 11 * 60, 30, 22.0, 0, 0) for i in range(40)]
with open(sys.argv[1], "w") as specs, open(sys.argv[2], "w") as man:
    for role, start, secs, kmh, hr, w in rides:
        specs.write("/rides/%s %d %d 45.70000 -121.52000 60 %.1f %d %d\n" % (name(start), start, secs, kmh, hr, w))
        man.write("%s %s %d %d %.1f %d %d\n" % (role, name(start), start, secs, kmh, hr, w))
EOF
"$BIN" card-tool fit "$RIDES" <"$RIDES/specs.txt" >/dev/null

# The app's log ([ble], [link], [sim-ble] lines), for reading after a run.
start_app_log() {
    xcrun simctl spawn "$UDID" log stream --style compact \
        --predicate 'process == "OpenTrailPaper" AND (eventMessage CONTAINS "[ble]" OR eventMessage CONTAINS "[link]" OR eventMessage CONTAINS "[sim-ble]")' \
        >"$1" 2>&1 &
    LOG_PID=$!
}

SIM_PID=
CDN_PID=
LOG_PID=
cleanup() {
    kill $SIM_PID $LOG_PID $CDN_PID 2>/dev/null || true
    [ -n "$SIM_PID" ] && wait "$SIM_PID" 2>/dev/null || true
    SIM_PID= CDN_PID= LOG_PID=
}
trap cleanup EXIT INT TERM

# One class (or one test of it) against a factory-fresh device: $1 = the
# xcodebuild -only-testing suffix ("Class" or "Class/test").
run_one() {
    DIR=$TOP/$(echo "$1" | tr '/' '-')
    mkdir -p "$DIR"
    LOG="$DIR/otp-sim.log"

    # 2. A factory-fresh device.
    "$BIN" --dir "$DIR/device" --ble-port "$BLE" --ctl-port "$CTL" --no-stdin --quiet >"$LOG" 2>&1 &
    SIM_PID=$!
    i=0
    until printf '{"cmd":"status"}\n' | nc -w 1 127.0.0.1 "$CTL" 2>/dev/null | grep -q '"ok":true'; do
        i=$((i + 1))
        if [ $i -gt 50 ] || ! kill -0 "$SIM_PID" 2>/dev/null; then
            echo "otp-sim did not come up; log: $LOG" >&2
            cat "$LOG" >&2
            return 1
        fi
        sleep 0.2
    done
    echo "== $1: otp-sim pid $SIM_PID: simble $BLE, control $CTL, state $DIR"
    # The firmware's idle auto-shutdown (10 min) would power the device off
    # while xcodebuild is still building on a slow runner.
    printf '{"cmd":"wait_log","pattern":"boot: all tasks started","since":0,"timeout_ms":30000}\n' |
        nc -w 31 127.0.0.1 "$CTL" | grep -q '"ok":true' || { echo "otp-sim did not finish booting" >&2; return 1; }
    printf '{"cmd":"console","line":"autosleep off"}\n' | nc -w 3 127.0.0.1 "$CTL" | grep -q 'autosleep: OFF' ||
        { echo "otp-sim did not take autosleep off" >&2; return 1; }

    # The stand-in tile CDN and Overpass: the map tests launch the app pointed at
    # it (-sim-tiles / -sim-overpass), so nothing reaches the internet.
    python3 sim/cdn/tile_cdn.py --dir "$FIXTURE" --port "$CDN" >"$DIR/tile_cdn.log" 2>&1 &
    CDN_PID=$!
    i=0
    until grep -q "listening" "$DIR/tile_cdn.log" 2>/dev/null; do
        i=$((i + 1))
        if [ $i -gt 50 ] || ! kill -0 "$CDN_PID" 2>/dev/null; then
            echo "tile_cdn.py did not come up" >&2
            cat "$DIR/tile_cdn.log" >&2
            return 1
        fi
        sleep 0.1
    done
    echo "== $(cat "$DIR/tile_cdn.log")"

    # 3. A fresh app (no remembered device, no bond) on a booted simulator.
    xcrun simctl boot "$UDID" 2>/dev/null || true
    xcrun simctl bootstatus "$UDID" -b >/dev/null
    xcrun simctl terminate "$UDID" "$BUNDLE" 2>/dev/null || true
    xcrun simctl uninstall "$UDID" "$BUNDLE" 2>/dev/null || true
    # The app streams the phone's position once connected; no prompt mid-test.
    xcrun simctl privacy "$UDID" grant location "$BUNDLE" 2>/dev/null || true
    # The phone is in the map fixture's area (the map tests also set it).
    CENTER=$(python3 -c 'import json,sys; c=json.load(open(sys.argv[1]))["center"]; print("%s,%s" % tuple(c))' "$FIXTURE/area.json" 2>/dev/null || true)
    [ -n "$CENTER" ] && xcrun simctl location "$UDID" set "$CENTER" 2>/dev/null || true
    start_app_log "$DIR/app.log"

    # 4. The tests. TEST_RUNNER_* reaches the test runner's environment.
    set +e
    TEST_RUNNER_OTP_SIM_BLE_PORT=$BLE \
    TEST_RUNNER_OTP_SIM_CTL_PORT=$CTL \
    TEST_RUNNER_OTP_SIM_RADIO_PORT=$RADIO \
    TEST_RUNNER_OTP_CDN_PORT=$CDN \
    TEST_RUNNER_OTP_SIM_DIR=$DIR/device \
    TEST_RUNNER_OTP_RIDE_FIXTURES=$RIDES \
    xcodebuild test \
        -project companion-ios/OpenTrailPaper.xcodeproj \
        -scheme OpenTrailPaper \
        -destination "id=$UDID" \
        -derivedDataPath "$DD" \
        -only-testing:"OpenTrailPaperUITests/$1" \
        -test-timeouts-enabled YES \
        -resultBundlePath "$DIR/results.xcresult" \
        ${OTP_XCODEBUILD_ARGS:-} \
        2>&1 | tee "$DIR/xcodebuild.log" | grep -E '^(Test Case|Test Suite|\*\*|.*error:|.*: error|.*XCTAssert|    Executed)'
    ONE_RC=$(grep -q '\*\* TEST SUCCEEDED \*\*' "$DIR/xcodebuild.log" && echo 0 || echo 1)
    set -e
    curl -s "http://127.0.0.1:$CDN/_ctl/log" >"$DIR/tile_cdn_requests.json" 2>/dev/null || true
    echo "== $1 logs: $DIR (otp-sim.log, app.log, tile_cdn_requests.json, xcodebuild.log, results.xcresult)"
    cleanup
    return "$ONE_RC"
}

RC=0
for run in $RUNS; do
    run_one "$run" || RC=1
done
echo "== logs: $TOP"
exit "$RC"
