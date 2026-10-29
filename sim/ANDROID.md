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
ShowPage pages the device; and regressions for app link bugs: two uploads /
downloads at once queue instead of failing BUSY, two ride downloads both
arrive intact, the ride list never blanks across a reconnect, the phone's GPS
stream stops at once for a device that already has a fix, a setting edited
offline reaches the device on reconnect, Bluetooth switched off and on comes
back connected. Bluetooth off in SimRadio reports the adapter
change before the links drop, as Android does. Each test's SimRadio log and the sim console tail
are in the test report's standard output.

### Maps

`MapsSimTest` runs the map flow end to end without the network:
`runMapDownload` (map/MapDownload.kt — what the Maps screen's Download /
Redownload buttons run: CDN index, tile and POI caches, PrebuiltTiles, the
Overpass fallback) fetches the fixture area from the stand-in CDN, and the
real BleManager sends it to the sim over SimRadio. Each test starts
`sim/cdn/tile_cdn.py` (python3) on a free port serving
`sim/fixtures/appmaps` (seven real tiles, see its README), and points the app
at it through the debug override below. What the device holds is read from
the device (control `tiles`, sim/PROTOCOL.md) and its card.

- `downloadedAreaReachesTheDeviceWithItsVersions`: every tile from the CDN
  (no Overpass), in the app's caches with `cdn:<hash>`; the device holds
  exactly those tiles and .poi files, with the CDN's version records and
  bytes, and its map screen at a fix inside the area is no longer
  "NO MAP HERE" (screenshot).
- `newCdnVersionResendsOnlyThatHex`: the CDN publishes a new version of one
  hex; Redownload fetches and sends that one only, and the device records the
  new version.
- `redownloadOfCurrentAreaSendsNothing`: nothing changed: "All 7 hexes are up
  to date.", only index.json is read, the device saves nothing.
- `badTilesFromTheCdnAreReportedAndNotSent`: one tile truncated, one a 500.
  Neither reaches the device or the cache; the fallback goes to the stand-in's
  Overpass (503) and the download reports the failure; the rest are sent.

The area is the fixture's list of cells (with their bboxes) rather than an H3
cover of a drawn box. The app keeps CDN indexes for an hour; the update tests
clear that cache as an hour passing would. The harness both test classes use
is `MapSimHarness`.

### Cycling POIs

`PoiSimTest` sends the fixture's POI sets (sim/fixtures/appmaps/pois: made-up
water, toilets, repair stands and bike shops, each cell's `.poi` built by the
reference writer, docs/mapgen.js buildPoi), which the stand-in serves on
request (`/_ctl/pois?set=`), and checks the device's card holds exactly the
reference bytes:

- `areaPoisReachTheDeviceByteForByte`: the CDN's .poi files, and the empty
  file the app synthesises for a cell listed with no POIs; CDN version
  records; each file fetched once, no Overpass.
- `poiChangeUpstreamResendsOnlyThatHex`: one cell's POIs change on the CDN;
  the Maps screen's version check wants POIs for that hex only, and "Send
  POIs" fetches and sends that one file (no tile, no other .poi).
- `areaWithNoPoisGetsEmptyPoiFiles`: nothing fetched, an empty .poi per hex.
- `garbledCdnPoisNeverReachTheDevice`: .poi bodies of the right size with
  every byte flipped are refused (fall back to Overpass, 503); the tiles and
  the empty .poi files still go, the screen says the POIs could not be fetched.
- `overpassPoisReachTheDeviceByteForByte`: the CDN's .poi files fail (500);
  the app builds them on the phone from the stand-in's Overpass answer, and
  they match the reference writer's bytes. Needs H3 (the cell of each POI):
  `HostH3` builds the app's own JNI library (src/main/cpp) for the host into
  sim/build/h3jni, which app/build.gradle.kts puts on the unit tests'
  java.library.path.
- `truncatedOverpassPoisNeverReachTheDevice`, `overpassErrorPageNeverReachesTheDevice`,
  `overpassTimeoutNeverReachesTheDeviceAsNoPois`: Overpass's POI answer is
  half a JSON, an HTML error page, or its 200 "runtime error" timeout remark
  with no elements. None reaches the device or the app's POI cache.
- `waterLayerSwitchHidesAndShowsTheIcon`: the device draws the water point
  150 m north of the rider; switching the water layer off in the app
  (`updateMapLayers`, the device's `settings.map_layers`) removes that icon
  and nothing else, and on brings it back.

### Dashboard and settings

`DashSettingsSimTest` drives the Dashboard editor's model (DashConfig /
DashLayout, what DashboardEditorSheet hands `saveDashConfig`) and the
Settings screen's `BleManager.updateX` setters (no Compose UI tests), and
reads back what the device applied (control `status`: `dash`, `page_count`,
`screen`, `settings`; `usbdrive` on the console). `FirmwareSim.reboot()`
restarts the firmware (control `reboot`) and reattaches the control channel.

- `richLayoutFromTheEditorIsWhatTheDeviceShows`: every page kind, the map
  second, a 75 % bottom radar rail, half rows, a custom map strip: the device
  holds the editor's bytes, seven pages, ShowPage(1) is the map.
- `sentLayoutSurvivesADeviceRebootAndIsReadBack`,
  `handEditedLayoutOnTheCardIsWhatTheAppShows`: read back on a new link;
  the app's parse of a hand-written card file agrees with the device's.
- `deviceTakesSevenContentPagesPlusTheMap` (DeviceHello.dash_max_pages),
  `layoutWithNothingToDrawIsRefusedAndTheDeviceKeepsItsOwn` (PARSE_ERROR).
- `everySettingReachesTheDeviceAndSurvivesAReboot`,
  `outOfRangeSettingsShowWhatTheDeviceApplied` (FTP 9000 -> 500, wheel 100 ->
  800, ...), `deviceSideEditsReachTheApp` (console `wheel`, `usbdrive`).
- Known bugs, `@Ignore("BUG: ...")`: a refused layout stays pending
  ("Syncing layout…", Ride-tab paging off); a layout over dash_max_text is
  never sent nor reported; eight data pages on the card lose the map; a
  compact card file over 2 KB once padded reaches the app cut off.

### The tile CDN override (debug builds)

`files/otp-endpoints` (`map/DebugEndpoints.kt`, src/debug; the release copy
is a stub that returns null) sends map downloads to another tile CDN and one
Overpass endpoint, e.g. the stand-in:

    sim/cdn/tile_cdn.py --dir sim/fixtures/appmaps --port 7923
    adb shell run-as com.raemond.opentrailpaper sh -c \
        'printf "tiles=http://10.0.2.2:7923/v1/\noverpass=http://10.0.2.2:7923/api/interpreter\n" > files/otp-endpoints'

Read at start. Debug builds allow plain HTTP to 10.0.2.2 / localhost only
(src/debug/res/xml/network_security_config.xml).

## The debug app on an emulator

    sim/build/otp-sim --dir /tmp/otpsim-android --ble-port 7878 --ctl-port 7879
    adb shell run-as com.raemond.opentrailpaper sh -c 'echo 10.0.2.2:7878 > files/otp-sim'
    adb shell am force-stop com.raemond.opentrailpaper   # then launch it

Add `forget` after the address to clear the sim phone's bonds (a phone that
forgot the device). Remove the file to go back to Bluetooth:
`adb shell run-as com.raemond.opentrailpaper rm files/otp-sim`. The app still
asks for the Bluetooth permissions (BleManager's gate); the sim needs no radio.
