# OpenTrailPaper — iOS Companion App

SwiftUI app that pairs with the OpenTrailPaper device over BLE to control
settings and push routes.

## What it does

- **Ride** — live device status over BLE: connection, speed, battery, heart
  rate, power, GPS lock, and active-route distance remaining.
- **Route** — search a destination with Apple Maps, build a route, preview
  it on the map, and send it to the head unit. The route is exported as GPX
  and streamed over BLE; the device saves it to `/routes` (and rides it
  even without an SD card).
- **Settings** — edit FTP and timezone, push them to the device (saved in
  the device's flash, persist across reboots).

## Build & run

Requires Xcode 16+ and [XcodeGen](https://github.com/yonaskolb/XcodeGen)
(`brew install xcodegen`).

```sh
cd companion-ios
xcodegen generate          # produces BikeGPSCompanion.xcodeproj
open BikeGPSCompanion.xcodeproj
```

Set your development team in the target's Signing settings, then run on an
iPhone (BLE needs a real device; the simulator has no Bluetooth). The app
auto-scans for the device by its GATT service UUID on launch.

## BLE protocol (matches `src/ble_server.cpp`)

Service `B1C50000-9E0F-4B7A-9C6D-1F2E3A4B5C6D`:

| Characteristic | UUID suffix | Access | Payload |
|----------------|-------------|--------|---------|
| Settings | `…0001` | read/write | `int16 ftpW, int16 tzMin` (LE) |
| Status   | `…0002` | notify | `u8 flags, u8 batt, u8 sats, u8 hr, u16 power, u16 speed×10, u16 remainKm×10` |
| Route    | `…0003` | write | framed: `0x01`+name, `0x02`+gpx chunks, `0x03` end |

## Notes / limitations

- Apple offers true cycling directions only in select regions; elsewhere
  the route falls back to walking geometry (closest to bike-friendly).
  There's a single `transportType` line in `RouteView.swift` to change, and
  a natural seam to drop in a dedicated bike router later.
- Route upload is paced (~12 ms/packet) for reliability over BLE; a long
  cross-town route of a few thousand points takes a couple of seconds.
