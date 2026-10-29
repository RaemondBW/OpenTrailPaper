# Firmware sim protocols

The sim (`sim/build/otp-sim`) runs the real firmware on the Mac. It listens on
two TCP ports on 127.0.0.1:

| Port (default) | Name    | Who connects                     | Format              |
|----------------|---------|----------------------------------|---------------------|
| 7878           | simble  | the phone: an app or a test      | binary frames       |
| 7879           | control | the test driving the device      | JSON lines          |

## simble — the phone's BLE link

One TCP connection is one BLE connection. Closing the socket is the phone
dropping the link (HCI reason 0x13). The device's GATT server, pairing and
bonding are the firmware's own (`src/ble_server.cpp`, `src/link.cpp`); the sim
models the radio's behaviour around them: connection, encryption, bonds, MTU,
CCCDs, attribute permissions and notify back-pressure.

Every frame: `u16 len (LE)` `u8 type` `body`, where `len` counts type + body.
UUIDs are 16 bytes in string order (`0000180d-0000-1000-8000-00805f9b34fb`
for a 16-bit UUID). Addresses are 6 bytes, most significant first (as
printed: `aa:bb:cc:dd:ee:ff`). Integers are little-endian.

### Phone → device

| Type | Name            | Body |
|------|-----------------|------|
| 0x01 | CONNECT         | `u8 version=1` `u16 mtu` `u8 flags` `6B identity` `u8 nameLen` `name` |
| 0x02 | DISCOVER        | — |
| 0x03 | READ            | `16B chr` |
| 0x04 | WRITE           | `16B chr` `u8 withResponse` `data` |
| 0x05 | SUBSCRIBE       | `16B chr` `u16 cccd` (1 notify, 2 indicate, 0 off) |
| 0x06 | SECURE          | — (start encryption or pairing now, as a phone does after an "insufficient authentication" error; on a link already encrypted with a bond, a key refresh: the firmware sees another authentication-complete) |
| 0x07 | DISCONNECT      | `u8 reason` |
| 0x08 | PASSKEY         | `u32 passkey` (what the rider typed) |
| 0x0A | ADV             | — (asks for the current advertisement) |
| 0x10 | PEER_READ_RSP   | `u32 reqId` `u8 status` `data` |
| 0x11 | PEER_NOTIFY     | `16B svc` `16B chr` `data` |
| 0x12 | PEER_SERVICES   | repeated `16B svc` `16B chr` `u32 props` |

CONNECT flags:

- bit 0 — the phone holds a bond with this device (iOS/Android "paired").
- bit 1 — pair if the device asks (a phone with no bond starting pairing).
- bit 2 — confirm the passkey automatically (the sim enters the code the panel
  shows, as a rider would). Without it the device sends PASSKEY_REQ (the
  phone's passkey dialog, which appears as soon as pairing starts — before the
  firmware's passkey callback runs) and waits for PASSKEY.
- bit 3 — the phone's bond holds keys that don't match the device's (bit 0
  set): encryption fails even though the device holds a bond.

The identity is the phone's identity address (its IRK-resolved address). A
test that wants to be "the same phone" across connections reuses it.

### Device → phone

| Type | Name            | Body |
|------|-----------------|------|
| 0x81 | CONNECTED       | `u16 handle` `u16 mtu` |
| 0x82 | DISCOVERED      | repeated `16B svc` `16B chr` `u32 props` |
| 0x83 | READ_RSP        | `16B chr` `u8 status` `data` |
| 0x84 | WRITE_RSP       | `16B chr` `u8 status` |
| 0x85 | SUBSCRIBE_RSP   | `16B chr` `u8 status` |
| 0x86 | NOTIFY          | `16B chr` `data` |
| 0x87 | DISCONNECTED    | `u16 reason` (HCI reason, e.g. 0x05 refused, 0x08 timeout, 0x13, 0x16) |
| 0x88 | PASSKEY_REQ     | — (the panel shows a code; send PASSKEY) |
| 0x89 | SERVICE_CHANGED | — |
| 0x8A | SECURITY        | `u8 encrypted` `u8 bonded` `u8 authenticated` `u8 keySize` |
| 0x8B | ADV             | `u8 advertising` `u8 nameLen` `name` `u8 mfgLen` `mfg` `u8 n` `n × 16B uuid` |
| 0x90 | PEER_READ       | `u32 reqId` `16B svc` `16B chr` |
| 0x91 | PEER_SUBSCRIBE  | `16B svc` `16B chr` `u16 cccd` |
| 0x92 | PEER_WRITE      | `16B svc` `16B chr` `data` |

Statuses are ATT errors: 0 ok, 0x05 insufficient authentication, 0x0F
insufficient encryption, 0x03 not permitted, 0x0A not found.

### Connection sequence

1. Phone: CONNECT. Device: CONNECTED (the firmware's `onConnect` has run).
2. Security starts when the firmware asks for it (its AMS code calls
   `startSecurity()` right after connecting) or when the phone sends SECURE.
   It runs as NimBLE + iOS/Android would run it:
   - phone bonded, device holds the bond → SECURITY(encrypted, bonded,
     authenticated) and `onAuthenticationComplete`.
   - phone bonded, device has no bond (it was unpaired), or the keys don't
     match (flag bit 3) → encryption fails: SECURITY(0,…) and the firmware
     decides what happens to the link.
   - phone not bonded, flags bit 1, device holds a bond for its identity (the
     phone forgot the device) → the firmware's pairing guard may refuse; if it
     doesn't, NimBLE-Arduino deletes the old bond and pairing runs.
   - phone not bonded, flags bit 1 → pairing: the firmware's passkey callback
     runs (the code appears on the panel), PASSKEY_REQ unless bit 2, then the
     bond is stored and SECURITY follows. A wrong PASSKEY fails pairing.
   - phone not bonded, no bit 1 → the link stays unencrypted; protected
     attributes answer 0x05.
3. Phone: PEER_SERVICES (optional: what its own GATT server has — the GAP
   device name `2a00` and Apple Media Service for AMS), DISCOVER, SUBSCRIBE,
   then reads, writes and notifications.

The firmware may drop the link at any point (refusal, unpair, OTA reboot):
DISCOVERED… DISCONNECTED, then the socket closes.

SERVICE_CHANGED is an indication: it reaches a link only once its CCCDs are
restored, i.e. after the firmware's `onAuthenticationComplete` has returned
for an encrypted, bonded link.

Bonds carry an IRK, so `ble_gap_unpair` / `deleteBond` fail with
BLE_HS_EBUSY (deleting nothing) while the device is advertising or scanning,
as on the controller.

### Back-pressure

`notify()` fails (returns false, as NimBLE does when it is out of buffers)
while more than `--notify-window` (default 32) notifications are queued for
the socket. With `--conn-interval-ms N --packets-per-event K` the writer
releases at most K notifications per N ms, which reproduces the slow-link
stalls a phone sees on a 300 ms interval.

## control — driving the device

Newline-delimited JSON. Every request may carry `"id"`; the response echoes it.
Responses are `{"id":…, "ok":true, …}` or `{"id":…, "ok":false, "error":"…"}`.
Asynchronous events (log lines, when subscribed) come as `{"event":…}`.

| cmd | arguments | does |
|-----|-----------|------|
| `status` | — | screen, page, settings (`map_layers`: the map layer mask, src/map_view.h), ride state, turn-by-turn (`nav`: prompt, next turn and the distance to it), phone link, advertisement, scanning, bonds, the card's size and use (`sd`, as last counted), panel counts (`paints`, `scrubs` — the scoped pre-paint resync, `clears` — whole-panel), clocks |
| `console` | `line`, `wait_ms` | runs a serial console command; returns its output |
| `screenshot` | `path` | writes the panel as a 540×960 PNG |
| `tiles` | `poi` | the map tiles (or, with `poi`, the .poi files) on the card as the device lists them to a phone, each with the version record it keeps: `{"tiles":[{"id":…,"version":"cdn:…"},…]}` (`""`: none) |
| `tap` | `x`, `y`, `ms` | a touch at panel coordinates (portrait, 540×960) |
| `button` | `name` (`boot`, `home`, `side`), `ms` | presses a key |
| `gps` | `lat`, `lon`, `alt`, `speed_kmh`, `course`, `fix`, `sats`, `time` (no fix: still report UTC; default true); or `track` [[lat,lon],…] with `speed_kmh` | sets what the GPS module reports |
| `gps_silent` | `on` | the GPS module goes quiet (sends and hears nothing) until `on: false` |
| `sensor` | `kind` (`hr`, `power`, `csc`, `speed`, `cadence`, `radar`), `addr`, `name`, `make`, `model`, `advertising`, `rssi`, values (`bpm`, `watts`, `cadence`, `speed_kmh`, `targets`), `sending`, `drop`; see [Virtual sensors](#virtual-sensors) | adds or updates a virtual BLE sensor |
| `sensor_remove` | `addr` | the sensor goes away (out of range) |
| `sensor_state` | — | what the firmware made of its sensors: `state` (RideState's sensor values, their ages, the speed source, the radar tracks) and `links` (per kind: paired address(es), live address, name, connected) |
| `media_state` | — | the now-playing store the MUSIC page draws: track, playing, `position_ms`, `duration_s`, `rate`, `ams` (AMS running), `ams_live`, album art `art_w`/`art_h`, `art_hash` (FNV-1a of the panel tones, -1 none) and `art_tones` (pixels per tone, black → white) |
| `battery` | `pct`, `charging` | the fuel gauge's reading |
| `wallclock` | `unix` | sets the device's clock (as the RTC would) |
| `sd` | `present`; or `latency_ms`; or `size_mb` (+ `cluster_kb`); or `usage` | inserts or removes the card; or makes every card open/read/write cost that much device time (a slow card); or swaps in a card of that size with the same files; or just reports. Answers `sd`: `card_bytes`, `total_bytes`, `used_bytes`, `free_bytes`, `cluster_bytes` (usage recounted from `<dir>/sd`) |
| `usb` | `host` | a computer mounts the card over USB (`true`: the firmware keeps off it) or lets it go (`false`: the firmware rescans its maps) |
| `speed` | `factor` | runs the device clock N× real time |
| `log` | `since` | log lines after sequence number `since` |
| `wait_log` | `pattern` (regex), `timeout_ms`, `since` | waits for a log line |
| `subscribe_log` | `on` | streams log lines as events |
| `reboot` | — | restarts the firmware (NVS and card kept): a power cut — RTC memory lost, the next boot is a power-on |
| `link_pace` | `interval_ms`, `packets`, `window` (each optional) | changes the phone link's pace live, as `--conn-interval-ms` / `--packets-per-event` / `--notify-window` set it at start (0 interval: unpaced); answers the values in force |
| `segments` | — | the live Strava segments as the SEGMENT page sees them (ui_dashboard's matcher, via `host/tu/ui_dashboard.cpp`): `count` (accepted set, `-1` none), `loaded` (adopted by the UI task), `phase` (`idle`/`armed`/`active`/`done`/`abandoned`), `busy`, and `list` — per relevant segment `name`, `phase`, `elapsed_ms`, `done_m`, `len_m`, `pr_s`, `delta_ms` (+ behind PR pace; `null` with no PR), `new_pr`, `off_route`, `avg_power`, `avg_hr`, `nearest`, `nearest_m` |
| `lora` | `loss_pct`, `crc_error_pct`, `latency_ms`, `rssi`, `snr`, `busy`, `overrun`, `seed` (all optional) | the LoRa radio (`--lora`, see below): sets the link's impairments, then reports `lora`: `fitted`, `up`, `mode` (off/sleep/standby/rx/tx), its tuning (`freq_mhz`, `bw_khz`, `sf`, `cr`, `sync_word`, `power_dbm`), the impairments, and counters — `queued`, `lost` (to `loss_pct`), `mistuned` (sent on another frequency/modem), `deaf` (arrived while it was asleep or transmitting), `overrun` (overwritten in the one-packet FIFO before it was read), `crc`, `read`, `tx`, `cad_busy`, `begins`, `tx_seq` — and for diagnosis `listening`, `deaf_spans` (recent [from, to] device-ms spans it was not receiving), `last_deaf_frame`, `now_ms`. With `airtime_len` N: just the time on air of N bytes at the current tuning |
| `lora_rx` | `hex`; `rssi`, `snr`, `delay_ms`, `elapsed_ms`, `crc_error`; `freq_mhz`, `bw_khz`, `sf`, `cr`, `sync_word` | puts a raw frame (1–255 bytes, a Meshtastic packet as sent over the air) on the air for the radio, by default on the tuning it has now. It is on the air for its LoRa time on air after the link latency, and after any frame before it (frames don't overlap); `elapsed_ms`: it started that long ago. Answers `lost` (dropped by `loss_pct`), `start_ms`, `end_ms` (device time) |
| `lora_tx` | `since` | frames the radio transmitted after sequence `since`, logged as each starts: `frames` [{`seq`, `start_ms`, `end_ms`, tuning, `power_dbm`, `aborted`, `hex`}], `seq` (the last), `now_ms` |
| `quit` | — | exits the sim |
| **power and robustness** | | |
| `reset` | `reason` (`panic`, `task_wdt`, `int_wdt`, `wdt`, `brownout`, `sw`, `ext`) | the chip resets for that reason: `esp_reset_reason()` says so on the next boot and RTC memory (`RTC_NOINIT_ATTR`: the crash reporter's breadcrumbs, diag's storage trace) survives. Unflushed card writes are lost, as with `reboot` |
| `gps_time` | `unix` | sets the GPS module's clock (UTC s); it keeps counting from there (a UTC midnight, a week-number rollover) |
| `gps_raw` | `data` | puts bytes on the GPS UART as they are (noise, bad checksums, torn or made-up sentences); also while `gps_silent` |
| `gps_paced` | `on` | the module's sentences arrive one by one, each after its bytes have crossed the UART, instead of as one batch a second |

`status` carries `reset_reason`: this boot's `esp_reset_reason()` (1 power-on,
3 software, 4 panic, 5 interrupt watchdog, 6 task watchdog, 7 watchdog,
8 wake from deep sleep, 9 brownout).

Resets in the sim: a reset re-execs it. `<dir>/rtc_mem.bin` carries the reset
reason and the `RTC_NOINIT_ATTR` section across (absent: a power-on).
`esp_restart()` is a software reset; the firmware's `abort()` (the console's
`crashlog panic`, a failed assert) is a panic; `--sd-crash-on-read` an
interrupt watchdog. Deep sleep exits the process, and the next launch of the
same directory is the wake (`ESP_RST_DEEPSLEEP`). The GPS module's clock runs
on across all of these (`<dir>/gps_clock.txt`, its backup supply); its fix and
track do not, so a test sets them again after a reset.

## Virtual sensors

`sensor` creates a peripheral the first time an `addr` is used (each kind has
a default address) and updates it after. It advertises its kind's service
and a Device Information service (`make`, `model`), and while the device is
connected and `sending` is true it notifies standard packets: once a second
(radar: 4 Hz) from cumulative counters that advance with `speed_kmh` /
`cadence`.

| kind | service | packets |
|------|---------|---------|
| `hr` | 0x180D | Heart Rate Measurement; `hr_wide` → 16-bit value, `contact` true/false/null → contact bits |
| `power` | 0x1818 | Cycling Power Measurement with crank data (`crank: false` drops it); `balance` (%), `torque`, `power_wheel` add those optional fields before it |
| `csc` / `speed` / `cadence` | 0x1816 | CSC Measurement with wheel and crank / wheel / crank data; CSC Feature says the same unless `feature` (a number; `null` leaves the characteristic out) |
| `radar` | Varia 6a4e3200 | header + 3 bytes per track; past six tracks the rest follow in continuation packets (header + 2). `targets`: `[m, …]` (ids by position) or `[[id, m], …]`; 0 m is "passed" |

Counters and shape: `wheel_mm` (the tyre the virtual wheel rolls on, default
2105), `wheel_revs` / `crank_revs` (set the cumulative counters, e.g. near
2^32), `reset: true` (counters back to zero, as after a battery swap),
`adv_uuids` (replace what the advert lists, e.g. `["fe1f"]`). `drop: true`
ends the link with a supervision timeout but keeps advertising;
`sensor_remove` takes it out of range altogether. Virtual sensors belong to
the sim process: `reboot` re-execs it, so a test puts them back afterwards.

Status blocks for workouts, the stopwatch and the frontlight (cov/workouts):

- `workout`: the WORKOUT page's model (`workout_service::view`, as the link's
  WorkoutStatus): `loaded`, `running`, `paused`, `done`, `name`, `segment`
  (0-based), `segments`, `elapsed_s`, `total_s`, `segment_left_s`,
  `target_w`, `next_w`, `ftp`, `pause_each`, and `picker` — the device-side
  picker's cached list (`state` scanning / ready / no_card, `total`,
  `loading`, `error`, and its first page's `rows`: `file`, `title`, `ok`,
  `total_s`, `segments`).
- `stopwatch`: the STOPWATCH page's clock: `running`, `elapsed_ms`, `laps`,
  `lap_ms` (the lap in progress).
- `backlight_pwm`: the frontlight's PWM duty (the last `analogWrite` on
  BOARD_BL_EN; -1 before the first).

A device that switches itself off (auto-sleep, SHUT DOWN) ends the process
(`[sim] powered off: …`); `SimDevice.wait_powered_off()` waits for it.

### The LoRa radio and its air

`--lora` fits the SX1262 (the mesh, `src/mesh_service.cpp`); without it the
board has none, as before. `sim/host/lora_sim.cpp` implements `lora_radio.h`
as the chip behaves: a transmission lasts its time on air (Semtech's formula)
and DIO1 fires at its end; the radio is half-duplex (what ends while it
transmits, sleeps or retunes is lost — `deaf`; after a transmission it receives again only when the firmware calls `listen()`/`finishSend()`, as the chip drops to standby at TxDone); a frame is heard only on the
same frequency, bandwidth, spreading factor and sync word (the coding rate is
in the LoRa header); the FIFO holds one packet, so one not read before the
next arrives is overwritten (`overrun`, as continuous receive does on the
chip); a damaged frame fires DIO1 and reads as nothing (`crc`). Channel-activity
detection is busy while a frame is on the air or `busy` is set. Collisions
between two senders are not modelled: frames queue one after another.

The air is the control channel. A test is a virtual node (`lora_rx`) and a
listener (`lora_tx`); `otpsim.mesh.Air` joins several sims by relaying each
one's transmissions to the others' `lora_rx` as they start, with per-link
loss/RSSI. `--mac` gives each sim its own MAC, i.e. its own mesh node number
(bytes 2–5, little-endian, as Meshtastic derives it).

## Command line

`otp-sim --help` lists the options. `--lora` fits the LoRa radio and
`--mac AA:BB:CC:DD:EE:FF` sets the board's MAC (above). Those about the card: `--sd DIR` (its
root, default `<dir>/sd`), `--sd-size-mb N` (capacity, default 32 GB),
`--sd-cluster-kb N` (FAT cluster size), `--sd-latency-ms N` (device time per
card call), `--sd-ready-ms N`, `--sd-crash-on-read PATH`.

`otp-sim card-tool` builds card contents with the firmware's own code (used by
`otpsim/cards.py`):

- `card-tool h3 S W N E` prints the res-6 H3 cells (map tiles) covering the
  box, one per line: `<id> <s> <w> <n> <e>`.
- `card-tool fit SDROOT` reads ride specs from stdin, one per line —
  `<card path> <start unix> <seconds> <lat> <lon> <bearing> <km/h> <hr|0> <watts|0>`
  — and writes each as a finished FIT activity with `FitWriter`.
