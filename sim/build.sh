#!/bin/sh
# Builds the firmware sim: the real firmware sources from src/ on the host,
# over sim/shim (Arduino, FreeRTOS, ESP-IDF, SD, NVS, NimBLE) and sim/host
# (the sim's stand-ins for hardware modules, and its main). Output:
# sim/build/otp-sim. Needs the vendor/ libraries checked out (TinyGPS++,
# epdiy's headers), as the firmware build does.
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
OUT=sim/build
mkdir -p "$OUT/obj"
VENDOR=${OTP_VENDOR:-$ROOT/vendor}
[ -d "$VENDOR/T5S3-4.7-e-paper-PRO" ] || { echo "vendor/ missing (set OTP_VENDOR)"; exit 1; }
EPDIY=$VENDOR/T5S3-4.7-e-paper-PRO/lib/epdiy/src
GPS=$VENDOR/T5S3-4.7-e-paper-PRO/lib/TinyGPSPlus/src

INC="-I sim/shim -I sim/host -I src -I companion-ios/Sources/H3/include -I src/proto -I third_party/nanopb -I $GPS -I $EPDIY -I tools/preview/shim -I companion-ios/Sources/H3"
DEFS="-DOTP_SIM=1 -DPB_ENABLE_MALLOC=0 -DARDUINO=10819 -DESP32=1 -DBOARD_HAS_PSRAM=1"
WARN="-Wno-unused-parameter -Wno-unused-variable -Wno-unused-function -Wno-deprecated-declarations -Wno-c99-designator -Wno-writable-strings -Wno-format -Wno-unused-but-set-variable"
CXXFLAGS="-std=gnu++17 -g -O1 $WARN $INC $DEFS"
CFLAGS="-std=gnu11 -g -O1 $WARN $INC $DEFS"

# Firmware sources compiled as-is. Everything else in src/ is hardware and
# has a stand-in in sim/host/.
FW_CPP="link settings dash_config dash_layout ui_dashboard ui_render vfont
  map_view map_tiles map_store routes segments ride_recorder fit_writer workout
  workout_service diag diag_sd media radar mesh_service mesh_proto mesh_crypto
  gps_service ble_server ble_sensors ams_client
  crash_report"
FW_C="link_frame proto/link.pb"
EXTRA_C="third_party/nanopb/pb_common third_party/nanopb/pb_encode third_party/nanopb/pb_decode
  companion-ios/Sources/H3/h3shim"
H3_C=$(ls companion-ios/Sources/H3/lib/*.c | sed 's/\.c$//')

# Objects are rebuilt when their source, any header in src/ or sim/, or this
# script is newer; compiles run in parallel.
objs=""
NEWEST_HDR=$(ls -t src/*.h src/proto/*.h sim/shim/*.h sim/shim/*/*.h sim/host/*.h sim/build.sh 2>/dev/null | head -1)
JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}
compile() {   # src, compiler, flags, ext
    o="$OUT/obj/$(echo "$1" | tr '/' '_').o"
    objs="$objs $o"
    if [ ! -f "$o" ] || [ "$1.$4" -nt "$o" ] || [ "$NEWEST_HDR" -nt "$o" ] || [ -n "$FORCE" ]; then
        while [ "$(jobs -p | wc -l)" -ge "$JOBS" ]; do sleep 0.05; done
        ( $2 $3 -c "$1.$4" -o "$o.tmp" 2> "$o.log" && mv "$o.tmp" "$o" ) || \
            { echo "FAIL $1.$4"; head -${ERRLINES:-15} "$o.log"; rm -f "$o.tmp"; touch "$OUT/failed"; } &
    fi
}
rm -f "$OUT/failed"
# POSIX calls on the card's VFS mount (opendir("/sd/rides")…) go to the sim's card.
for f in $FW_CPP; do compile src/$f clang++ "$CXXFLAGS -include sim/shim/sim_posix.h" cpp; done
# The rasteriser half of epd_compat (the panel half is host/epd_sim.cpp), built
# as the preview tool builds it.
compile src/epd_compat clang++ "$CXXFLAGS -UARDUINO" cpp
for f in $FW_C; do compile src/$f clang "$CFLAGS" c; done
for f in $EXTRA_C $H3_C; do compile $f clang "$CFLAGS" c; done
compile "$GPS/TinyGPS++" clang++ "$CXXFLAGS" cpp
for f in sim/host/*.cpp; do [ -f "$f" ] && compile "${f%.cpp}" clang++ "$CXXFLAGS" cpp; done
for f in sim/host/*.c; do [ -f "$f" ] && compile "${f%.c}" clang "$CFLAGS" c; done
wait
[ -f "$OUT/failed" ] && exit 1
clang++ $objs -lz -o "$OUT/otp-sim"
echo "built $OUT/otp-sim"
