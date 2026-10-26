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
| E-paper panel | a framebuffer; every paint is counted, screenshots are PNGs of what's on the glass |
| Touch, BOOT/side/home keys | injected from the control channel; a press is held until the firmware has read it |
| SD card | a directory (`<dir>/sd`), removable |
| NVS | files under `<dir>/nvs`, kept across reboots |
| L76K GPS on UART2 | an emulated module: obeys PCAS commands (baud, output, version, save), keeps its baud across restarts, streams NMEA for a fix or a track; a wrong UART baud reads garbage |
| BLE stack + radio | [nimble_sim.cpp](host/nimble_sim.cpp): NimBLE's API over TCP. Connections, pairing (the firmware's passkey callback), bonds (kept in `<dir>/bonds.txt`), encryption, attribute permissions, CCCDs, MTU, notify back-pressure and slow connection intervals |
| BLE sensors | virtual peripherals sending standard HR / Cycling Power / CSC / Varia radar packets, which the firmware's own sensor code scans for, pairs, decodes and loses |
| The phone's GATT server (AMS, name) | provided by whatever connects as the phone |
| OTA | the image lands in `<dir>/ota.bin`; the reboot re-execs the sim |
| Fuel gauge | set from the control channel |
| LoRa, Qwiic sensors, RTC chip, USB drive, light sleep | absent (the firmware's "not fitted" paths) |

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

## Testing the apps

The apps reach the sim through a debug-only radio that speaks simble instead
of CoreBluetooth / BluetoothGatt, so their own connection logic — pairing,
refusals, reconnects, backgrounding — runs against the real firmware. See
[IOS.md](IOS.md) and [ANDROID.md](ANDROID.md).

## Writing tests that fail for the right reason

A test that passes with the bug present proves nothing. When a test guards a
fix, check it against the unfixed code once: `test_download_survives_a_slow_link`
fails with the old 320 ms notify retry budget and passes with the fix.

## Known gaps

- Timing is approximate: a sped-up clock scales delays, not CPU work.
- No radio-level effects beyond what's modelled (range, interference, the
  controller's own scheduling).
- The panel's waveforms and refresh modes aren't modelled: a paint is instant.
- A phone can connect while the device isn't advertising (the controller
  would refuse it), so a second central during a live phone link is always
  possible here.
- Light sleep, BT modem sleep and power_mgmt.cpp aren't built: link drops
  caused by sleep clocks are out of reach.
