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
| 0x06 | SECURE          | — (start encryption or pairing now, as a phone does after an "insufficient authentication" error) |
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
  shows, as a rider would). Without it the device sends PASSKEY_REQ and waits
  for PASSKEY.

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
   - phone bonded, device has no bond (it was unpaired) → encryption fails:
     SECURITY(0,…) and the firmware drops the link (DISCONNECTED 0x05).
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
| `status` | — | screen, page, settings, ride state, turn-by-turn (`nav`: prompt, next turn and the distance to it), phone link, advertisement, bonds, panel counts (`paints`, `scrubs` — the scoped pre-paint resync, `clears` — whole-panel), clocks |
| `console` | `line`, `wait_ms` | runs a serial console command; returns its output |
| `screenshot` | `path` | writes the panel as a 540×960 PNG |
| `tap` | `x`, `y`, `ms` | a touch at panel coordinates (portrait, 540×960) |
| `button` | `name` (`boot`, `home`, `side`), `ms` | presses a key |
| `gps` | `lat`, `lon`, `alt`, `speed_kmh`, `course`, `fix`, `sats`, `time` (no fix: still report UTC; default true); or `track` [[lat,lon],…] with `speed_kmh` | sets what the GPS module reports |
| `gps_silent` | `on` | the GPS module goes quiet (sends and hears nothing) until `on: false` |
| `sensor` | `kind` (`hr`, `power`, `csc`, `speed`, `radar`), `addr`, `name`, `advertising`, values (`bpm`, `watts`, `cadence`, `speed_kmh`, `targets`) | adds or updates a virtual BLE sensor |
| `sensor_remove` | `addr` | the sensor goes away (out of range) |
| `battery` | `pct`, `charging` | the fuel gauge's reading |
| `wallclock` | `unix` | sets the device's clock (as the RTC would) |
| `sd` | `present`; or `latency_ms` | inserts or removes the card; or makes every card open/read/write cost that much device time (a slow card) |
| `speed` | `factor` | runs the device clock N× real time |
| `log` | `since` | log lines after sequence number `since` |
| `wait_log` | `pattern` (regex), `timeout_ms`, `since` | waits for a log line |
| `subscribe_log` | `on` | streams log lines as events |
| `reboot` | — | restarts the firmware (NVS and card kept) |
| `quit` | — | exits the sim |
