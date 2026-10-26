#!/bin/sh
# The Android app's BleManager against the firmware simulator (sim/ANDROID.md).
# Builds the sim if needed, then runs the Robolectric sim tests. Extra
# arguments go to Gradle (e.g. --info).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ -x "$ROOT/sim/build/otp-sim" ] || "$ROOT/sim/build.sh"
: "${JAVA_HOME:=/opt/homebrew/opt/openjdk@17}"
: "${ANDROID_HOME:=/opt/homebrew/share/android-commandlinetools}"
export JAVA_HOME ANDROID_HOME
cd "$ROOT/companion-android"
./gradlew -q :app:testDebugUnitTest --tests 'com.raemond.opentrailpaper.sim.*' "$@"
echo "sim tests passed (report: companion-android/app/build/reports/tests/testDebugUnitTest/index.html)"
