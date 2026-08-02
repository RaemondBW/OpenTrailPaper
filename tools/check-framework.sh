#!/usr/bin/env bash
# Verify the PlatformIO Arduino framework on this machine is the stock,
# unmodified one — by CONTENT, not by version string.
#
# Why this exists: the framework installs into an UNVERSIONED directory
# (~/.platformio/packages/framework-arduinoespressif32, no version suffix), so a
# hand-modified copy — e.g. a PM-enabled SDK overlay — sits in the same slot and
# still reports version 3.20014.231204. In July 2026 that produced binaries
# where the SD card would not mount on ANY firmware, including two-week-old
# commits, and a version audit said "clean" the whole time. See
# investigations/pm-rebuild-baseline.md.
#
# Usage:
#   tools/check-framework.sh          # verify against the recorded baseline
#   tools/check-framework.sh --print  # print current values (to re-record)

set -uo pipefail

FW="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/framework-arduinoespressif32"
SDK="$FW/tools/sdk/esp32s3"
VARIANT=qio_opi        # build.arduino.memory_type for T5-ePaper-S3

# ---- Recorded baseline: stock framework-arduinoespressif32 @ 3.20014.231204 --
# (arduino-esp32 2.0.14 / ESP-IDF 4.4.6), captured 2026-07-31 on a framework
# confirmed to build working firmware.
WANT_VERSION="3.20014.231204"
WANT_SDKCONFIG="b967289ef51c64186e65a3d30cb86199c3f48965064b6277e718a8c1fc644507"
WANT_ESP_PM="27fd6fe4ea5ebc49d11e26f3e255a08b41921bf130aa7696a9cf351e4dd92c00"
WANT_BUILDPY="4fd36c9a1f465d10f9d30cbfacdbfe2e5458636fc2c4c8e7ce8329889a55827f"
WANT_SDK_TREE="f69dc59175b685cf6238ea3a573cb63e84003017dc3e9e503e94e4af596f6a75"
WANT_VARIANT_TREE="bf4a9d912410aad104724116cbed5029d7e2002d09c1771c2705f8f23d753869"

sha() { shasum -a 256 "$1" 2>/dev/null | cut -d' ' -f1; }
tree_sha() { find "$1" -type f -exec shasum -a 256 {} \; 2>/dev/null | sort -k2 | shasum -a 256 | cut -d' ' -f1; }

if [ ! -d "$SDK" ]; then
    echo "FAIL: no framework at $FW"
    echo "      Install it:  pio pkg install -e t5s3-painter"
    exit 1
fi

got_version=$(python3 -c "import json,sys;print(json.load(open('$FW/.piopm'))['version'])" 2>/dev/null || echo "?")
got_sdkconfig=$(sha "$SDK/sdkconfig")
got_esp_pm=$(sha "$SDK/lib/libesp_pm.a")
got_buildpy=$(sha "$FW/tools/platformio-build-esp32s3.py")

if [ "${1:-}" = "--print" ]; then
    echo "WANT_VERSION=\"$got_version\""
    echo "WANT_SDKCONFIG=\"$got_sdkconfig\""
    echo "WANT_ESP_PM=\"$got_esp_pm\""
    echo "WANT_BUILDPY=\"$got_buildpy\""
    echo "WANT_SDK_TREE=\"$(tree_sha "$SDK")\""
    echo "WANT_VARIANT_TREE=\"$(tree_sha "$SDK/$VARIANT")\""
    exit 0
fi

fail=0
check() {  # name expected actual
    if [ "$2" = "$3" ]; then
        printf '  ok    %s\n' "$1"
    else
        printf '  DIFF  %s\n        expected %s\n        got      %s\n' "$1" "$2" "$3"
        fail=1
    fi
}

echo "framework: $FW"
check "version"                  "$WANT_VERSION"   "$got_version"
check "tools/sdk/esp32s3/sdkconfig" "$WANT_SDKCONFIG" "$got_sdkconfig"
check "lib/libesp_pm.a"          "$WANT_ESP_PM"    "$got_esp_pm"
check "platformio-build-esp32s3.py" "$WANT_BUILDPY" "$got_buildpy"

# The full-tree hashes take ~30 s over 2374 files; skip with --fast.
if [ "${1:-}" != "--fast" ]; then
    echo "  (hashing 2374 SDK files, ~30 s...)"
    check "tools/sdk/esp32s3/ (whole tree)" "$WANT_SDK_TREE"     "$(tree_sha "$SDK")"
    check "$VARIANT/ (live flash variant)"  "$WANT_VARIANT_TREE" "$(tree_sha "$SDK/$VARIANT")"
fi

# Independent semantic check: on a stock framework esp_pm_configure is a stub
# that returns ESP_ERR_NOT_SUPPORTED (0x106). On a PM-enabled rebuild it is a
# real function, hundreds of bytes long.
NM="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-nm"
if [ -x "$NM" ]; then
    # nm -S prints "<addr> <size> T <name>", both hex. BSD awk has no strtonum,
    # so convert in the shell.
    hex=$("$NM" -S --defined-only "$SDK/lib/libesp_pm.a" 2>/dev/null \
          | awk '$4=="esp_pm_configure"{print $2; exit}')
    if [ -z "$hex" ]; then
        echo "  DIFF  esp_pm_configure not found in libesp_pm.a"
        fail=1
    else
        size=$((16#$hex))
        if [ "$size" -le 16 ]; then
            echo "  ok    esp_pm_configure is a ${size}-byte stub (PM compiled out — stock)"
        else
            echo "  DIFF  esp_pm_configure is ${size} bytes — this is a PM-ENABLED framework"
            fail=1
        fi
    fi
fi

echo
if [ $fail -eq 0 ]; then
    echo "PASS — stock framework, safe to build."
else
    cat <<'EOF'
FAIL — this framework is NOT the recorded stock one.

If you are deliberately testing the PM build, fine: expect it, and revert with
    rm -rf ~/.platformio/packages/framework-arduinoespressif32
    rm -rf .pio                       # build cache AND libdeps, both
    pio pkg install -e t5s3-painter
then re-run this script until it says PASS.

Do NOT try to repair the framework in place. See
investigations/pm-rebuild-baseline.md § "Reverting".
EOF
fi
exit $fail
