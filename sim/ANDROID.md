# The Android app against the firmware sim

The app's real `BleManager` — connect, bond, refusal, barren-connect and
reconnect logic, `GattQueue`, `LinkClient` — runs against the real firmware in
`otp-sim` through a radio seam:

- `ble/Radio.kt` (main): the `Radio` / `RadioDevice` / `RadioGatt` interfaces
  BleManager talks to, and `AndroidRadio`, a one-for-one pass-through to
  `BluetoothAdapter` / `BluetoothGatt` (release and normal debug builds).
- `ble/SimRadio.kt` (src/debug only): the same interfaces over simble
  (PROTOCOL.md), reporting what Android's stack would — GATT statuses (0x05
  refusal → 5, link loss → 8, MIC failure → 61, a connect that never reached
  the device → 133 with no STATE_CONNECTED), bond broadcasts
  (BONDING → BONDED / NONE), encryption on connect for a bonded phone. Its
  SharedPreferences are the phone's bond store and identity address.
- `ble/DebugRadio.kt`: debug picks SimRadio when `files/otp-sim` exists;
  the release copy returns null.

## Tests (no emulator needed)

    sim/run_android_tests.sh

or as part of `./gradlew :app:testDebugUnitTest`. `BleManagerSimTest`
(src/testDebug, Robolectric) launches `sim/build/otp-sim` on free ports with a
fresh device directory per test and is skipped when the sim isn't built (set
`OTP_SIM=/path/to/otp-sim` to use another binary). Scenarios: first pairing +
Hello; link drops reconnect without NOT_RECOGNISED; reconnects that never come
up (sim stopped) are not a refusal; a second phone is refused without looping;
ShowPage pages the device. Each test's SimRadio log and the sim console tail
are in the test report's standard output.

## The debug app on an emulator

    sim/build/otp-sim --dir /tmp/otpsim-android --ble-port 7878 --ctl-port 7879
    adb shell run-as com.raemond.opentrailpaper sh -c 'echo 10.0.2.2:7878 > files/otp-sim'
    adb shell am force-stop com.raemond.opentrailpaper   # then launch it

Add `forget` after the address to clear the sim phone's bonds (a phone that
forgot the device). Remove the file to go back to Bluetooth:
`adb shell run-as com.raemond.opentrailpaper rm files/otp-sim`. The app still
asks for the Bluetooth permissions (BleManager's gate); the sim needs no radio.
