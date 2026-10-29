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
- `SimEndpoints` (same file, `#if DEBUG`): map downloads from another tile
  CDN and one Overpass endpoint: `-sim-tiles <base URL>` (env
  `OTP_SIM_TILES`) replaces Info.plist's `OTPTileBaseURL`, `-sim-overpass
  <URL>` (env `OTP_SIM_OVERPASS`) the Overpass mirror list
  (`PrebuiltTiles.configuredBase`, `MapBuilder.overpassPost`). A Release
  build has neither.

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
| Bluetooth off (Darwin notification `com.raemond.opentrailpaper.sim.bluetooth-off`, `SimBluetooth.set(on:)` in the UI tests) | `.poweredOff`, the link dropped with no `didDisconnect`; `-on` reports `.poweredOn` |

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
7920/7921) and the stand-in tile CDN (`sim/cdn/tile_cdn.py` on 7923, serving
`sim/fixtures/appmaps`, or `OTP_CDN_FIXTURE`), reinstalls the app on
simulator `OTP_IOS_SIM`, runs `companion-ios/UITests/FirmwareSimTests.swift`
and stops both. Logs (otp-sim, the app's `[ble]`/`[link]`/`[sim-ble]` lines,
the CDN's requests, xcodebuild, the result bundle) are kept in the temp dir
it prints.

The tests drive the device over the control port (`UITests/SimHarness.swift`,
`SimControl`), and the app reaches simble through `RangeProxy` (port 7922), a
relay in the test process that plays the radio path: `connectFails` makes
every connect fail before the link is up without the device hearing of it,
the "device at the edge of range" case.

### Maps

The map tests launch the app with `-sim-tiles` / `-sim-overpass` pointing at
the stand-in CDN (`TileCdn` in SimHarness.swift), and the phone's location
(`XCUIDevice.location`) at the fixture's centre, so the Maps screen opens on
it. They go through the UI as a rider does: Settings → Maps on SD card →
Select area → drag a box in the middle of the map → Download. What the device
then holds is read from the device (control `tiles`).

- `test06_TruncatedTilesAreReportedAndNotSent`: every tile is truncated by the
  CDN. None reaches the device; the app falls back to Overpass (the stand-in:
  503) and the Maps screen reports the failure.
- `test07_MapAreaDownloadReachesTheDevice`: the device holds exactly the hexes
  the Download button offered, all fixture hexes, with the CDN's version
  records (tile and .poi); no Overpass request; and its map screen at a GPS
  fix in the area goes from "NO MAP HERE" to a drawn map (screenshot, kept in
  the result bundle).
- `test08_NewCdnVersionIsRedownloaded` (after test07): the CDN publishes a new
  version of one hex on the device; the Maps screen offers "Redownload 1 hex",
  only that tile is fetched, and the device records its new version.

Cycling POIs, after test09 (test08's tiles stay on the device), from the fixture's POI sets
(sim/fixtures/appmaps/pois, each cell's `.poi` built by the reference writer
docs/mapgen.js; the stand-in serves one on `/_ctl/pois?set=`). The tests read
the device's card (`OTP_SIM_DIR`, which run_ios_tests.sh passes to the test
runner) and the reference files from the stand-in (`/_ctl/poi`: a runner
reading under ~/Documents would wait on a privacy prompt). They draw their box
around the rider (the screen's locate button: with tiles on the device the
Maps screen frames their coverage instead), so it holds the centre hex, the
one the POI sets edit. The device is checked with the app closed, and each
failure is printed (`FirmwareSimTests FAIL`) before it is recorded, since
XCTest's failure triage can hang on the Maps screen:

- `test10_AreaPoisReachTheDeviceAndAreDrawn`: the CDN gets POIs for the area;
  "Send POIs for N hexes" offers exactly the hexes whose .poi changed, the
  device holds every hex's .poi byte for byte with the CDN's records, only
  those .poi files are fetched (no tile, no Overpass), and the device's map
  now draws the water point 150 m north of the rider (screenshot diff).
- `test11_PoiChangeUpstreamIsSentForThatHexOnly`: one cell's POIs change
  upstream; "Send POIs for 1 hex" fetches and sends that .poi only.
- `test12_WaterLayerSwitchHidesTheIcon`: Maps → Device map layers →
  Drinking water off reaches the device (`settings.map_layers`) and removes
  that icon and nothing else; on brings it back.
- `test13_OverpassTimeoutNeverReachesTheDeviceAsNoPois`: the CDN's .poi fails
  (500) and Overpass answers with its 200 "runtime error" timeout remark; the
  screen says the POIs could not be fetched and the device keeps what it had.
- `test14_OverpassPoisAreBuiltOnThePhone`: the CDN's .poi fails; the app
  builds it from the stand-in's Overpass answer, and the device holds the
  reference writer's bytes, recorded `phone:…`.

The fixture covers the area around the centre generously (±0.04° / ±0.05°),
so the box, however the map frames it, gets only fixture hexes; a box outside
it fails at once, naming the group index the screen looked up. The tests
reset the app's location authorization and allow the prompt, since an
earlier test may have answered it.

Known gap: when the Maps screen gets into some states (the selection off the
fixture, so every hex falls to the failing Overpass; a broken tile or a
wrong version record on the device, as with the app code deliberately
broken), XCUITest's wait for the app to idle never returns, and the test ends
on its 4-minute allowance rather than an assertion. While the download runs,
a thread watches the device and prints `FirmwareSimTests device watch: FAIL:
…` to the log when a bad tile arrives, so the reason is in xcodebuild.log
even then.

### Routes

`testN01_GpxImportedThroughFilesReachesTheDevice`: Route → Import a route → GPX file
→ the system document picker → On My iPhone → `lost_lake_loop.gpx` → Send to
device. run_ios_tests.sh copies `sim/fixtures/routes/*.gpx` into the
simulator's Files storage (the `group.com.apple.FileProvider.LocalStorage`
container) before the run. The device must hold the file's 400 track points
(not its waypoints) as `routes/lost_lake_loop.gpx`, loaded, 6 km long, and the
app must say the device received it.
