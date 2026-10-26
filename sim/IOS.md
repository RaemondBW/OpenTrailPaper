# The iOS app on the firmware sim

A DEBUG build of the iOS app can use the firmware sim (`otp-sim`) as its
Bluetooth radio, in the iOS Simulator. BLEManager's connection state machine
(scan, connect, pairing, refusal, barren-connect counting, reconnect,
background/foreground) and LinkClient run unchanged on top; only the radio
underneath differs.

## How

- `companion-ios/Sources/Radio.swift`: `RadioCentral` / `RadioPeer`, the part
  of CoreBluetooth BLEManager drives. `CBCentralManager` and `CBPeripheral`
  conform directly (no wrappers). `RadioEvents` is what the radio reports;
  BLEManager's CoreBluetooth delegate methods are thin adapters onto it.
- `companion-ios/Sources/SimRadio.swift` (`#if DEBUG`): the same protocols over
  TCP to simble (PROTOCOL.md). Chosen at launch by `-sim-ble host:port` or env
  `OTP_SIM_BLE`; otherwise the app uses CoreBluetooth. `-sim-forget-bond` is
  iOS's "Forget This Device".

SimRadio behaves as iOS does, so BLEManager sees what it would on a phone:

| Situation | What the app sees |
|-----------|-------------------|
| sim not running (connection refused) | nothing: the connect stays pending and is retried quietly |
| bonded phone connects | `didConnect` after the link is encrypted (SECURE → SECURITY) |
| bonded phone, device lost the bond (SECURITY encrypted=0) | `didFailToConnect` `.peerRemovedPairingInformation` |
| unbonded phone | `didConnect` at CONNECTED, then pairing (passkey auto-confirmed, CONNECT bit 2) |
| socket closes before the link is up | `didFailToConnect` `.connectionFailed` |
| DISCONNECTED 0x08 | `didDisconnect` `.connectionTimeout` |
| DISCONNECTED (any other reason) | `didDisconnect` `.peripheralDisconnected` |
| socket closes with no DISCONNECTED (e.g. `ble_drop`) | `didDisconnect` `.connectionTimeout` (supervision timeout) |
| `cancelPeer` (active or pending) | `didDisconnect` with no error |

The bond (phone identity address, "bonded" flag, peripheral identifier) is
kept in the app's UserDefaults (`sim.ble.*`). Scanning asks the device for its
advertisement (ADV) once a second. Only the link service is reported; the
legacy characteristics never appear (firmware with the link doesn't need them).
In the background the app holds a background task, standing in for
bluetooth-central waking it for CoreBluetooth's events.

## Running

```sh
sim/build/otp-sim --dir /tmp/otpsim-ios --ble-port 7920 --ctl-port 7921 --no-stdin
xcrun simctl launch <udid> com.raemond.opentrailpaper -didOnboard YES -sim-ble 127.0.0.1:7920
```

## UI tests

```sh
sim/run_ios_tests.sh                 # all of FirmwareSimTests
sim/run_ios_tests.sh test03_BackgroundLinkLossReconnectsOnForeground
```

The script builds the sim if needed, starts a factory-fresh one (ports
7920/7921), reinstalls the app on simulator `OTP_IOS_SIM`, runs
`companion-ios/UITests/FirmwareSimTests.swift` and stops the sim. Logs
(otp-sim, the app's `[ble]`/`[link]`/`[sim-ble]` lines, xcodebuild, the
result bundle) are kept in the temp dir it prints.

The tests drive the device over the control port (`UITests/SimHarness.swift`,
`SimControl`), and the app reaches simble through `RangeProxy` (port 7922), a
relay in the test process that plays the radio path: `connectFails` makes
every connect fail before the link is up without the device hearing of it,
the "device at the edge of range" case.
