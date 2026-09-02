# OpenTrailPaper — Android Companion App

Jetpack Compose app that pairs with the OpenTrailPaper device over BLE to control
settings, push routes (searched on the phone or imported from a `.gpx`) and
build offline maps. Feature-for-feature with [`companion-ios`](../companion-ios)
on device control and maps, against the same firmware and the same `.ebm` tile
format.

## What it does

- **Ride** — live device status over BLE: connection, speed, battery, heart rate,
  power, GPS lock, and active-route distance remaining. The dashboard card opens
  the layout editor.
- **Route** — search a destination and build a cycling route, or import a `.gpx`
  (file picker, or opened from another app); preview it and send it to the head
  unit. The route is exported as GPX and streamed over BLE; the device saves it
  to `/routes` (and rides it even without an SD card). A searched route comes
  with turn cues built in; an imported route draws, tracks progress and reports
  remaining distance without them, unless turn cues are added afterward (derived
  from OSRM). Hexagons appear over any part of the route neither the phone nor
  the device has map coverage for.
- **Rides** — lists recorded `.fit` files, downloads them over BLE, decodes them
  (distance, moving time, power, normalized power, HR, DEM ascent) and shows the
  track on a map. Downloaded rides stay cached for offline viewing and can be
  shared to Strava, Files or anything else installed.
- **Maps** — drag a box; the app covers it with H3 res-6 hexagons (~5.6 km
  across), skips the ones already on the device, fetches OpenStreetMap for the
  rest, bakes roads/water/coastline/parks/elevation into `.ebm` tiles and streams
  each new one to the SD card. Areas you've downloaded are shaded as hexagons —
  green with a check where the device has them too — so the screen answers which
  ground you hold and whether it is on the head unit.
- **Mesh** — text messaging over the head unit's LoRa radio, which makes it a
  real [Meshtastic](https://meshtastic.org) node. Broadcast to the channel or
  message one neighbour, with delivery marks and the nodes that have reported a
  position plotted on a map. Private channels carry a random 256-bit key and are
  shared by QR code in Meshtastic's own `meshtastic.org/e/#` format, so the
  official app can join one and this app can join theirs.
- **Settings** — units, clock format, USB-drive mode, FTP, timezone, backlight;
  sensor pairing; device diagnostics logs with a battery-drain chart; and
  over-the-air firmware updates straight from GitHub Releases.
- **Dashboard editor** — reorder and resize the fields the panel shows, with a
  preview that runs the firmware's own layout algorithm in device pixels.

## Build & run

Requires JDK 17, the Android SDK (platform 35, build-tools 35), CMake 3.22.1 and
NDK r26 — the last two build the vendored H3 C library, which is shared verbatim
with the iOS app so a hexagon gets the same id on both phones.

```sh
cd companion-android
./gradlew :app:installDebug     # or open the folder in Android Studio
```

`local.properties` needs `sdk.dir=…` pointing at your SDK (Android Studio writes
it for you).

Run on a real phone: BLE needs one, and the emulator has no Bluetooth. The app
auto-scans for the device by its GATT service UUID once Bluetooth is allowed.

## How it differs from the iOS app

Both companions do the same jobs; four implementation choices differ because the
platforms do.

- **Maps, search and routing.** iOS uses MapKit throughout. Android has no
  equivalent that doesn't need an API key, so this uses **osmdroid** for the map
  (the same OpenStreetMap data the offline tiles come from), **Nominatim** for
  destination search and the FOSSGIS-hosted **OSRM** instances for directions.
  Cycling is the primary routing profile everywhere, with walking as the fallback
  — a small improvement on iOS, which falls back to walking geometry wherever
  Apple has no cycling coverage. All three services are free and keyless; searches
  fire on submit rather than per keystroke, and thumbnails are cached, to stay
  inside their usage policies.
- **BLE flow control.** CoreBluetooth lets you fire writes freely and reports when
  the pipe drains. Android's stack accepts exactly one outstanding GATT operation
  and silently drops anything issued before the previous callback lands, so every
  read, write and descriptor write goes through a serialized queue
  (`ble/GattQueue.kt`). The transfer pumps hand it a whole payload and let it
  drain — the queue *is* the flow control.
- **Background location.** iOS toggles `allowsBackgroundLocationUpdates` while a
  ride records. Android needs a location-typed foreground service for the same
  thing, so `ble/RideLocationService.kt` runs — and only runs — while the device
  reports that it is recording.
- **Mesh channel QR codes.** iOS draws them with CoreImage and reads them with
  AVFoundation, both in the OS. Android's equivalents are in Play Services (ML
  Kit), which this app does not depend on for the same reason it does not use
  Google Maps — so the QR is drawn and decoded by **ZXing**'s pure-Java core, with
  **CameraX** supplying the frames. The camera is bound to the scanner's own
  lifecycle, so it runs only while that sheet is open, and the permission is
  optional: the invite link can be pasted instead.

## BLE protocol (matches `src/ble_server.cpp`)

Service `B1C50000-9E0F-4B7A-9C6D-1F2E3A4B5C6D`:

| Characteristic | UUID suffix | Access | Payload |
|----------------|-------------|--------|---------|
| Settings | `…0001` | read/write/notify | `i16 ftpW, i16 tzMin, u8 miles, u8 backlight, u8 clock24, u8 usbDrive` (LE) |
| Status   | `…0002` | notify | `u8 flags, u8 batt, u8 sats, u8 hr, u16 power, u16 speed×10, u16 remainKm×10` |
| Route    | `…0003` | write/notify | framed: `0x01`+name, `0x02`+gpx chunks, `0x03` end track, `0x04`+turn, `0x05` end nav, `0x08`+GPS aiding |
| Rides    | `…0004` | write/notify | list / download / delete, plus the per-day log files |
| OTA      | `…0005` | write/notify | `0x01`+size+md5, `0x02`+chunks, `0x03` commit |
| Sensors  | `…0006` | write/notify | scan / stop / pair / forget / snapshot |
| Map      | `…0007` | write/notify | whole-map and per-tile uploads, plus the on-device tile list |
| Dash     | `…0009` | read/write/notify | the text of `/config/dashboard.cfg` |
| Mesh     | `…000a` | write/notify | Meshtastic state, messages, nodes, channels and modem presets, both ways |

## Notes / limitations

- Nominatim and the FOSSGIS OSRM instances are volunteer-run. Heavy use will be
  rate-limited; a self-hosted instance is a one-line change in
  `routing/Routing.kt`.
- Elevation grids come from Open-Meteo (free, no key, 100 points per request), so
  a large map download makes a lot of small requests. Tiles are cached on disk, so
  a retry after a dropped link costs nothing.
- Map building is memory-hungry: an Overpass region is streamed straight into
  primitive arrays rather than a JSON tree (`map/OsmData.kt`), which is what keeps
  a large selection inside a phone's heap.
