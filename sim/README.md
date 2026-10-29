# Firmware sim

The real firmware on a Mac, so the device and the phone apps can be tested
without hardware, in seconds, by a script.

```
sim/build.sh                       # → sim/build/otp-sim (≈5 s, parallel, incremental)
sim/build/otp-sim --dir /tmp/dev   # boot a device; its console is on stdout/stdin
sim/test.sh                        # the firmware suite (pytest)
```

## What runs, and what is simulated

`otp-sim` compiles `src/` as it is — the UI and renderer, the phone link
(`link.cpp`), the GATT server and fixed pairing (`ble_server.cpp`), the sensor
client (`ble_sensors.cpp`), AMS, GPS parsing and baud detection
(`gps_service.cpp`), the ride recorder and FIT writer, routes, maps, segments,
workouts, settings, diag. Only `src/main.cpp` is replaced, by
`sim/host/sim_main.cpp`, which mirrors its `setup()`/`loop()` minus the board.

Underneath it, `sim/shim/` stands in for Arduino, FreeRTOS, ESP-IDF, the SD
card, NVS and NimBLE, and `sim/host/` for the hardware:

| Hardware | In the sim |
|---|---|
| Tasks, mutexes, queues, ring buffers | threads ([rtos_sim.cpp](host/rtos_sim.cpp)); the clock can run N× faster |
| E-paper panel | a framebuffer; every paint, scrub and clear is counted, screenshots are PNGs of what's on the glass |
| Touch, BOOT/side/home keys | injected from the control channel; a press is held until the firmware has read it |
| SD card | a directory (`<dir>/sd`), removable, with a size (`--sd-size-mb`, default 32 GB): FAT32 clusters are counted and writes run short when it's full (see below); optionally slow (`--sd-latency-ms`, `sd latency_ms`), slow to come up after power-on (`--sd-ready-ms`), or holding a file whose reading resets the device (`--sd-crash-on-read`); a computer can hold it over USB (`usb host`) while a test edits the directory, and the firmware rescans its maps when it lets go |
| NVS | files under `<dir>/nvs`, kept across reboots |
| L76K GPS on UART2 | an emulated module: obeys PCAS commands (baud, output, version, save), keeps its baud across restarts, streams NMEA for a fix or a track; a wrong UART baud reads garbage |
| BLE stack + radio | [nimble_sim.cpp](host/nimble_sim.cpp): NimBLE's API over TCP. Connections, pairing (the firmware's passkey callback), bonds (kept in `<dir>/bonds.txt`), encryption, attribute permissions, CCCDs, MTU, notify back-pressure and slow connection intervals |
| BLE sensors | virtual peripherals sending standard HR / Cycling Power / CSC / Varia radar packets, which the firmware's own sensor code scans for, pairs, decodes and loses |
| The phone's GATT server (AMS, name) | provided by whatever connects as the phone |
| OTA | the image lands in `<dir>/ota.bin`; the reboot re-execs the sim |
| Resets, RTC memory | a panic, watchdog or brownout (`reset`), `esp_restart()` and the firmware's `abort()` re-exec the sim with `esp_reset_reason()` set and `RTC_NOINIT_ATTR` memory kept; deep sleep exits it and the next launch is the wake. The crash reporter (`crash_report.cpp`) is the firmware's own |
| Fuel gauge | set from the control channel |
| LoRa, Qwiic sensors, RTC chip, the USB stack itself, light sleep, Memfault | absent (the firmware's "not fitted" paths) |

Two TCP ports, both described in [PROTOCOL.md](PROTOCOL.md):

- **simble** (7878): a phone connects here. One connection = one BLE link.
- **control** (7879): a test drives the device — status, taps, keys, GPS,
  sensors, the card, the clock, screenshots, the log, reboot.

## Testing the firmware

```
python3 -m venv sim/.venv && sim/.venv/bin/pip install "protobuf>=5,<7" pytest
sim/test.sh                     # everything
sim/test.sh -k pairing          # pytest's own options pass through
```

Each test boots a factory-fresh device in a temp directory on free ports.
A failing test leaves the console log and a screenshot of the panel in
`sim/build/test-artifacts/<test>/`.

The Python client ([client/otpsim](client/otpsim)) is the phone and the
rider:

```python
from otpsim import SimDevice, SimPhone, LinkClient, pb

with SimDevice.start() as dev:                    # boots otp-sim
    phone = SimPhone(dev.ble_port).connect()      # pairs (passkey auto-confirmed)
    phone.wait_security()
    link = LinkClient(phone).hello()              # the protobuf link
    link.tell(show_page=pb.ShowPage(page=1))
    dev.wait_status(lambda s: s["page"] == 1)
    dev.sensor("hr", bpm=150)                     # a virtual strap
    dev.gps_track(points, speed_kmh=30)           # ride along a track
    dev.button("boot")                            # start a ride
    dev.screenshot("panel.png")
```

## SD cards: real contents, a real size

The card is `<dir>/sd`, and it is as big as `--sd-size-mb` says (default
32 GB; `sd size_mb` changes it live). `SD.cardSize()`, `totalBytes()` and
`usedBytes()` report that card: usage is counted from the directory in FAT32
clusters (`--sd-cluster-kb`, default 32 KB from 2 GB up, else 16 KB), directory
entries and the FATs included, and is recounted on every `usedBytes()` and
mount, so files a test drops in are seen. Every write path the firmware has
goes through it — `fs::File` writes, truncating opens, removes and `mkdir`, and
the POSIX calls some firmware makes on the VFS mount (`opendir("/sd/rides")`,
`stat`, `fopen`), which `sim/shim/sim_posix.h` redirects to the card. A write
that needs a cluster the card hasn't got is cut short where the free space
ends (0 and `ENOSPC` when nothing fits), as FatFs's `f_write` does; a new
directory needs a free cluster; `rename` onto an existing name fails
(`FR_EXIST`).

[client/otpsim/cards.py](client/otpsim/cards.py) fills a card before boot,
in the firmware's own layout: `empty`, `typical` (150 rides, 12 routes,
4 workouts, ~200 Bay Area map tiles, 60 days of logs), `heavy` (1000 rides,
100 routes, ~5000 tiles, a year of big logs) and `nearly_full` (typical on a
128 MB card with 4 MB left). Rides are real FIT files from the firmware's
`FitWriter` and tile names real H3 cells, both via `otp-sim card-tool`. Tiles
are a small valid EBM2 built from `data/sf.ebm`; `cards.set_tile_source(fn)`
swaps in real ones.

```python
card = cards.build("typical", "/tmp/cards/typical")    # ~0.5 s (heavy: ~6 s)
card.install(dev_dir)                                   # an APFS clone: instant
dev = SimDevice(dir=dev_dir, args=card.sim_args)
dev.fill_card(leave_bytes=0)                            # and now it's full
```

`sim/tests/test_cards.py` runs the device against each profile and against a
card that fills up. The `heavy` profile and the ride that fills a card are
marked slow: `sim/test.sh --slow` runs them too (~2 min more).

## Testing the apps

The apps reach the sim through a debug-only radio that speaks simble instead
of CoreBluetooth / BluetoothGatt, so their own connection logic — pairing,
refusals, reconnects, backgrounding — runs against the real firmware. See
[IOS.md](IOS.md) and [ANDROID.md](ANDROID.md).

## Maps: real tiles

`sim/fixtures/tiles/` holds sixteen real H3 res-6 tiles around Hood River,
Oregon, as the CDN serves them (375 KB, OSM data under the ODbL — see its
README; `build_fixture.py` rebuilds it). `otpsim.tiles` hands them out
(`fixture_tiles()`, `fixture_pois()` → `[(h3, bytes, version)]`,
`fixture_manifest()` for the centre, bbox, a track and a point outside),
writes them straight onto a card directory (`install_on_card()`), reads
`.ebm` roads and projects them onto the map page as the firmware does.

[tests/test_maps.py](tests/test_maps.py) sends the region as the apps' map
download does and lists it back; draws it (ink against the "NO MAP HERE"
page, and the fixture's own streets must land on inked pixels — and must not
when the projection is moved 30 m); rides across tile boundaries; replaces a
tile with a newer version; deletes tiles over USB; times big transfers on a
normal and a slow link; and damages tiles twenty ways (grid, cell size, index
entries, record and point counts, elevation and water sections): the link
must refuse each, and each put on the card over USB must not take the device
down. The firmware's `timing` console switch logs every map frame (polys per
road class, tiles drawn of those wanted, ` cut` for a partial frame, `pois=`
kept/dropped), which the tests read too.

[tests/test_pois.py](tests/test_pois.py) does the same for the cycling POIs
(`.poi` next to each tile), with the synthetic sets of
`sim/fixtures/tiles/pois/` (`build_pois.mjs`: made-up OSM nodes encoded by the
real `docs/mapgen.js` buildPoi; `otpsim.tiles.poi_set()`): each icon on the
glass is compared with the one the MAP LAYERS screen draws for its type, at
the place the file puts it — per type and flag, across a cell edge, by zoom
and layer, replaced, emptied and deleted, past the frame's 256-POI budget,
damaged, and along a ride on a route.

## Power and robustness

[tests/test_power.py](tests/test_power.py) times the idle auto-off (ten
minutes, to about a second of device time, with the clock running fast and
slowing near the moment), what restarts its countdown (a tap, the keys,
`autosleep on`) and what holds it off (a ride recording, riding, a phone, the
stopwatch), the power dialog (CANCEL; SHUT DOWN mid-ride keeps a valid FIT
and the next boot is clean), and the battery as the app sees it.
[tests/test_crash_recovery.py](tests/test_crash_recovery.py) crashes,
watchdogs and power-cuts the device mid-ride (CONTINUE, SAVE, DISCARD; the
FIT valid and in order), checks the crash report each reset reason leaves on
the card for the app (with the last log lines before it), its wait in NVS for
a card and the queue through a crash loop, and a tile torn by a reset.
[tests/test_gps_edges.py](tests/test_gps_edges.py) covers a tunnel, a fix
that jumps across the map, line noise, a GPS week rollover and a ride across
UTC midnight. [tests/test_endurance.py](tests/test_endurance.py) (slow) rides
four hours watching memory.

## Routes, navigation and segments

[tests/test_navigation.py](tests/test_navigation.py) sends routes as the apps
do (GPX upload, cues, RouteFinish) and as a rider copies them onto the card,
then rides them with the GPS model: storage and naming, refusals, delete,
reload from NAVIGATE after a reboot, the countdown to each turn, out-and-backs,
detours and rejoining, joining midway, arrival, the NAVIGATE prompt, the cue
cap, very long tracks, recording while navigating, and the route's ridden /
ahead styles on the glass. [tests/test_segments.py](tests/test_segments.py)
syncs starred Strava segments (segment_match.h's blob) and rides through
them, reading the SEGMENT page's matcher through the control `segments`
command.

That command needs ui_dashboard.cpp's file-local matcher, so the sim builds
`sim/host/tu/ui_dashboard.cpp` — which `#include`s the firmware file unchanged
and adds read-only hooks after it — in place of `src/ui_dashboard.cpp`
(build.sh rebuilds it whenever the firmware file changes). Further hooks into
that file's state go there too.

`SimPhone.pace_writes(interval_ms, per_event)` paces the phone's own writes
like a connection interval; the sim's `--conn-interval-ms` paces only the
device's notifications.

## Sensors, radar and music

The virtual sensors (PROTOCOL.md, "Virtual sensors") send the standard
packets with their optional fields, counters that can be set near a wrap or
reset, and the radar's continuation packets; `sensor_state` reads back what
the firmware made of them and `media_state` the MUSIC page's store. An iPhone
with Apple Media Service is `SimPhone(...).offer_ams()`: the device's AMS
client subscribes over the phone's own GATT server, `ams_track` /
`ams_playback` send Entity Updates as iOS does, and `ams_commands()` lists the
Remote Commands the device wrote. `otpsim.fit` reads a ride back record by
record. Tests: [test_sensors.py](tests/test_sensors.py) (pairing on the panel
and from the app, several straps, reboot, packet formats, wheel size and
counter wraps, the speed source, dropouts on the glass and in the FIT),
[test_radar.py](tests/test_radar.py), [test_media.py](tests/test_media.py).

## Writing tests that fail for the right reason

A test that passes with the bug present proves nothing. When a test guards a
fix, check it against the unfixed code once: `test_download_survives_a_slow_link`
fails with the old 320 ms notify retry budget and passes with the fix. The
map tests were checked the same way: with the tile decoder's y axis flipped,
with saveTile no longer dropping the cached copy of a replaced tile, and with
the card rescan keeping its tile cache, the draw, ride, replace and delete
tests fail.

A firmware bug a test finds but doesn't fix is a strict `xfail` naming it,
so the fix turns it into a failure to remove the marker.

## Checks the board can't make

Two classes of bug only show on hardware by luck — a freeze when the timing
lines up — so the sim reports them whenever the code path runs, and the
tests assert the console is clean of them:

- `[sim] SD ACCESS WITHOUT sdLock: …` — a card call from a running task that
  doesn't hold `sdLock()`. On the board the card shares its SPI bus with the
  LoRa radio, and an unlocked call can wedge it.
- `[sim] LOCK ORDER INVERSION: …` — two mutexes taken in both orders (with
  `portMAX_DELAY`), i.e. an ABBA deadlock waiting for its interleaving.

## Known gaps

- The card costs no time unless asked (`--sd-latency-ms`), and then a flat
  cost per call: FAT's directory searches, which make opening a file by name
  grow with the directory, aren't modelled. Directory listings come back
  sorted by name (FAT: creation order).
- A full card fails the write that doesn't fit, immediately; on the board
  newlib's stdio buffer can defer the failure to a later write or the close.

- Timing is approximate: a sped-up clock scales delays, not CPU work.
- No radio-level effects beyond what's modelled (range, interference, the
  controller's own scheduling).
- The panel's waveforms and refresh modes aren't modelled: a paint is instant.
- A phone can connect while the device isn't advertising (the controller
  would refuse it), so a second central during a live phone link is always
  possible here.
- Light sleep, BT modem sleep and power_mgmt.cpp aren't built: link drops
  caused by sleep clocks are out of reach. Nor is the fuel gauge's own
  behaviour: `battery` sets what it reads, and the firmware has no low-battery
  action of its own to test (a flat battery is a `reset("brownout")`).
- The GPS module loses its fix and track across a reset (its clock is kept).
- NVS writes are atomic (a file written whole and renamed): a power cut can't
  tear a setting, so settings writes under power loss aren't tested.
- The heap is the host's malloc: `esp_get_free_heap_size()` is a constant, so
  a leak shows only as the process's resident memory (`SimDevice.rss_kb()`).
- Phone → device writes cost nothing unless the test paces them
  (`pace_writes`); BLE retransmissions and a full controller write queue
  aren't modelled.
- No address sanitizer: an out-of-bounds read only shows when it faults.
  A damaged tile's small overreads (a header with no index behind it) pass
  silently.
