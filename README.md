# Open E-Paper Bike Computer

DIY bike GPS head unit for the [LilyGO T5S3 4.7" e-paper PRO](https://github.com/Xinyuan-LilyGO/T5S3-4.7-e-paper-PRO).

The board has everything onboard: ESP32-S3 (16MB flash / 8MB PSRAM, BLE 5),
960×540 e-paper with GT911 touch, GPS (u-blox MIA-M10Q or L76K — both
supported, autodetected), SD card slot, PCF8563 RTC, BQ25896 charger and
BQ27220 fuel gauge.

## Features

- **GPS** — position, speed, heading, altitude, satellites, UTC time
  (L76K / u-blox M10Q autodetected)
- **BLE sensors** — heart rate, cycling power (incl. cadence from crank
  data), speed/cadence. Pair from the Sensors screen; pairings persist in
  flash. Unpaired kinds fall back to first-found.
- **Ride recording** — 1 Hz FIT files on the SD card (`/rides/*.fit`),
  uploadable to Strava / intervals.icu. Moving time, avg/normalized power,
  avg HR, and climbing tracked for the summary screen (SAVE / DISCARD).
- **Offline maps** — OSM converted by `tools/maps/build_map.py` into a
  compact tiled binary; San Francisco ships embedded in flash. 1-bit
  rendering, zoom 1-8 m/px, follows GPS.
- **GPX routes** — drop `.gpx` files in `/routes` on the SD card, pick one
  under Navigate. Route draws on the map (ridden solid / ahead dashed)
  with km-remaining in the footer.
- **Settings on device** — FTP (zone bar) and timezone, persisted in NVS.
- **Screens** — dashboard (power hero + grid), map, summary, menu,
  sensors, navigate, history, settings.

### Controls

- Tap the status bar → menu; tap elsewhere → dashboard ↔ map
- Long-press (1.2 s) → start / stop ride
- Map: +/− buttons zoom

## Roadmap

- Turn-by-turn hints + climb screen (design 1d/1e)
- Track-up map rotation, SD-card map regions beyond SF
- Per-zone partial refresh, auto-pause, deep sleep + resume

## Building

Requires [PlatformIO](https://platformio.org/) and the vendored LilyGO repo
(board definition + display/touch/battery drivers):

```sh
git clone --depth 1 https://github.com/Xinyuan-LilyGO/T5S3-4.7-e-paper-PRO vendor/T5S3-4.7-e-paper-PRO   # if missing
pio run                    # build
pio run -t upload          # flash over USB
pio device monitor         # serial log @115200
```

## Screen previews without hardware

`tools/preview/render_preview.sh` compiles the actual epdiy drawing/font
code for macOS (hardware layer stubbed) and renders the UI to PNGs in
`tools/preview/out/` — pixel-identical to the panel, including the planned
map view fed with a synthetic street scene. Iterate on layout there before
flashing.

## Layout

```
src/
  main.cpp           task startup, IO expander (GPS power), fuel gauge
  config.h           pin map + tunables
  ride_state.h       mutex-guarded shared state (GPS/BLE/battery → UI/recorder)
  gps_service.*      L76K / M10Q autodetect, TinyGPSPlus → state
  ble_sensors.*      NimBLE central: HR / power / CSC parsing
  fit_writer.*       minimal FIT activity encoder
  ride_recorder.*    ride lifecycle, distance, SD writes
  ui_dashboard.*     epdiy rendering + GT911 touch
  fonts/             FiraSans (from LilyGO factory firmware)
tools/               (planned) OSM → map tile converter
vendor/              LilyGO board support (cloned, not committed)
```

## SD card layout

```
/rides/YYYYMMDD-HHMMSS.fit   ride recordings (UTC timestamps)
/routes/*.gpx                routes for the Navigate screen
```
