# The phone link: one protobuf channel over BLE

Every app ↔ device feature runs over one GATT service with two
characteristics, carrying protobuf messages defined in
[`proto/link.proto`](../proto/link.proto). A new feature is a new message
and a `Feature` value, not a new characteristic. So there is no GATT
change for iOS to cache and no re-pairing.

The twelve characteristics in service `b1c50000` (SETTINGS … WORKOUT) are
the **legacy protocol**. Firmware keeps serving them for apps that predate the
link, and apps keep their legacy code for firmware that predates it (an app
has to be able to update old firmware). Once both sides have had a release with
the link, legacy code can go.

## GATT

| | UUID | Properties |
|---|---|---|
| Service | `b1c51000-9e0f-4b7a-9c6d-1f2e3a4b5c6d` | not advertised (the bike service `b1c50000` still is) |
| RX, phone → device | `b1c51001-…` | write, write without response; encrypted + authenticated |
| TX, device → phone | `b1c51002-…` | notify; CCCD encrypted + authenticated |

Same security as the legacy characteristics: an LE Secure Connections bond is
required for every write and for the CCCD, and the device ignores RX and
sends nothing on TX until the link is admitted (`phoneConnected`). The UUIDs
are the ones the unreleased LIVE service used. Its byte protocol is gone, and
the link replaces it.

The firmware still sends a Service Changed indication once per boot, so
phones that cached the older GATT table find the service.

## Framing

A protobuf message (`PhoneMsg` on RX, `DeviceMsg` on TX) is split into
fragments. Each fragment is one ATT write or notification:

```
byte 0     header: b7 FIRST, b6 LAST, b5-b4 reserved (0), b3-b0 fragment counter
byte 1..   up to MTU-3-1 bytes of the encoded message
```

- A message is the fragments from one with FIRST to one with LAST. A message
  that fits in one fragment has both bits set (header `0xC0 | counter`).
- The counter runs 0–15 per direction across all fragments, wrapping. A
  receiver that sees a gap, a FIRST in the middle of a message, or a
  continuation without a FIRST drops the partial message and logs it. The
  next FIRST starts clean. Writes and notifications do not get lost on a live
  link. The counter is there to catch bugs, not to retransmit.
- A sender never interleaves two messages. Each side has one writer, and a
  multi-fragment message goes out back-to-back. Priority happens *between*
  messages, not inside one (below).
- Maximum message size after reassembly is `DeviceHello.max_message` (4096 on
  current firmware). The phone has no limit. Every message the device sends
  is ≤ 2060 bytes (the nanopb `otp_link_DeviceMsg_size`).

### Sending priority

Each side keeps two queues: **control** (everything else) and **bulk**
(`Chunk`). The writer takes the next control message if there is one, and a
`Chunk` otherwise. Senders size `Chunk.data` so that the whole encoded
`PhoneMsg`/`DeviceMsg` fits one fragment:
`MTU - 3 (ATT) - 1 (header) - envelope and chunk fields`. Those fields take 16 bytes while `xfer` < 128 and offsets are < 256 MB, and 18 at worst. Compute them exactly or keep `xfer` small. That leaves 227 bytes
at MTU 247. So a live frame or a button press never waits behind more than
one chunk.

Phone writes: bulk chunks go out as write-without-response, paced by the
platform's flow control (`canSendWriteWithoutResponse` /
`peripheralIsReady` on iOS, GattQueue completion on Android). Control
messages may use either write type.

Device notifications: one sender (the link task) retries a busy `notify()`
like `sendChunk` does today.

## Session

1. The phone discovers `b1c51000`. If it is missing, use the legacy protocol.
2. Subscribe TX. When iOS/Android confirms the subscription, send
   `Hello{proto, platform, app_version, features}`. If no confirmation arrives
   within 2 s, send `Hello` anyway: a reply proves the subscription works.
3. The device answers `DeviceHello` (same `req`): firmware version, feature
   list, limits. Then it pushes the initial state without being asked:
   `Settings`, `DashConfig`, `MapLayers`, `WorkoutStatus`, `MediaTrack` (if
   AMS has one), and `LiveFrame`s with the RIDE topic at 1 Hz.
4. No `DeviceHello` within 3 s: resend `Hello` up to twice more, then fall
   back to the legacy protocol for this connection and log why.
5. In link mode the app does **not** subscribe to the legacy characteristics.
   The device sends each push on exactly one path: the link once `Hello`
   arrived on this connection, otherwise the legacy characteristic. Legacy
   writes are still accepted in link mode.

`Hello` is written with response, so iOS/Android hold it until pairing
completes instead of dropping it. A `DeviceHello` that arrives after the app
already fell back still switches the connection to the link, because the device
then pushes only there.

`Hello` again on the same connection is fine (an app restart under iOS state
restoration): the device answers and re-pushes the initial state. Link state
(subscription, live topics, transfers) is per connection and dies with it.

### Feature discovery

`DeviceHello.features` replaces every implicit check in the legacy protocol:
characteristic presence, settings length, MEDIA A1 bits, MAP silence vs.
E0/D5/flagged D0, the MESH 0x90 tail, the LIVE HELLO caps and the OTA version
reply. The app gates every UI affordance on a feature, never on the firmware
version string. `dash_max_pages` replaces the "holds 8 pages" capability bit.

## Requests and replies

- The phone gives every request a non-zero `req` that is unique on the
  connection (a counter). Messages that need no reply (`PhoneLocation`,
  `Chunk`, `TransferAck`, `StopwatchCommand`, `ShowPage`) may use 0.
- The device echoes `req` on every message that answers it, including each
  page of a multi-page list. Unsolicited pushes have `req = 0`. That includes
  pushes a request caused: LiveFrames after `LiveSubscribe`, SensorLists while
  scanning, and mesh topics refreshed after a config edit.
- A request without a specific answer message gets `Result` (`RESULT_OK` or an
  error code with an optional `detail`). Any request the device cannot decode,
  or whose body it doesn't know, gets `Result{RESULT_UNSUPPORTED}`.
- The device keeps a queue of requests, not one slot per kind, so the phone
  may pipeline. The legacy "single pending request" limits are gone.
  Requests of one kind are answered in order.

| Request | Answer |
|---|---|
| `GetSettings`, `Settings` (set) | `Settings` (full current values) |
| `LiveSubscribe` | `Result`, then `LiveFrame`s |
| `ListRequest` | `FileList` pages / `TileList` pages / `MapCoverageList` |
| `DeleteRequest` | `Result` |
| `UploadBegin` | `UploadReady` or `Result` error |
| `UploadEnd` | `TransferDone` |
| `DownloadRequest` | `DownloadBegin` or `Result` error |
| `RouteManeuvers` | `Result` |
| `RouteFinish` | `Result` (OK = track loaded, PARSE_ERROR = unusable GPX) |
| `SensorCommand` | `SensorList` (LIST/PAIR/FORGET), `Result` (SCAN_*), then pushes while scanning |
| `GetMapLayers`, `MapLayers` (set) | `MapLayers` |
| `GetDash`, `DashConfig` (set) | `DashConfig` (what the device applied) or `Result` PARSE_ERROR |
| `MediaMeta`, `MediaClear`, `MediaCommand` | `Result` |
| `WorkoutCommand` | `WorkoutStatus` (LOAD: `Result`) |
| `MeshCommand.get` | one message per topic (`MeshState`, `MeshHistory` pages, …) |
| `MeshCommand.send` | `MeshQueued` or `Result` REFUSED |
| other `MeshCommand` | `Result`, then the refreshed topics as pushes |
| `AgnssInfoRequest` | `AgnssInfo` |
| `GetSegments` | `SegmentsInfo` |
| `Unpair` | none: the device disconnects |

## Live state

`LiveFrame` replaces both STATUS and the LIVE stream. The phone chooses what
it wants with `LiveSubscribe{topics}`, which replaces the current set. RIDE is
on by default after `Hello` (the old 1 Hz status, which the app needs for fix
state and the ride summary). The Ride tab adds the topics for the page in view:
STOPWATCH for the stopwatch page, MUSIC for music, POSITION (+ VIEW) for the
map, RADAR when a radar shows. It drops them when the page or the tab goes away,
and drops everything except RIDE when the app goes to the background. There is
no keepalive: the subscription lasts until changed or until the connection ends.

The device sends a frame each second and on the next pass after any change to
a subscribed part (stopwatch state, view, music, page). Only subscribed parts
are filled. "No reading" is an absent `optional` field, not a sentinel value.

Paging the Ride tab pages the panel too: the app sends `ShowPage{page}` with
the slot it moved to (FEATURE_SHOW_PAGE), indexing the layout the device has
applied, never one still being saved. The device follows only while it shows a
page or the map, never over a menu, sheet, overlay or nav prompt, and ignores a
slot past the end. There is no reply; the next frame's `ViewState` shows where
the panel is. The app sends it on the rider's paging only, not on opening the
tab or while editing a layout.

`RideState.sd_card` reports the card: OK, MISSING (nothing in the slot) or
UNREADABLE (a card answered but won't mount, almost always exFAT). It is
UNKNOWN (0) from firmware before it was reported, which the apps must read as
"don't know", never as a missing card. It carries the same three states as the
legacy status flags' bits 3-5.

## Transfers

One mechanism for every byte stream. Firmware, maps, tiles, POI files, GPX
routes, workouts, album art, AGNSS and starred segments go up. Rides, logs
and workout files come down.

Offsets are absolute file positions everywhere. `Chunk.offset` is the
position in the whole file. `TransferAck.received` is the absolute position up
to which bytes arrived, which is offset + bytes received on a resumed download.

**Upload** (phone → device):

```
UploadBegin{xfer, target, name, size, crc32, …}  →
                                                ←  UploadReady{xfer, window}   (or Result: NO_MEMORY, BUSY, TOO_LARGE, RECORDING …)
Chunk{xfer, offset, data} …                     →   keep offset+len − acked ≤ window
                                                ←  TransferAck{xfer, received} at least every window/2
UploadEnd{xfer}                                 →
                                                ←  TransferDone{xfer, code, detail}
```

- `xfer` is chosen by the phone. One upload at a time on current firmware: a
  second `UploadBegin` while one runs gets `RESULT_BUSY`.
- The device checks `size` and CRC-32 (IEEE, as zlib `crc32`) on
  `UploadEnd` before saving anything. Firmware updates are therefore verified,
  unlike the legacy MD5 that was never checked.
- `TransferDone` comes once the content is parsed and saved. For a firmware
  image that is "saved, rebooting in 1.5 s". For a GPX route it is "parsed and
  stored", with PARSE_ERROR if the GPX was unusable. A commit that has to wait
  for the SD card (a ride recording, the computer using it as a USB drive) waits
  without a timeout, like the legacy protocol. The app shows "waiting".
- A chunk whose `offset` isn't the next byte, or that overruns `size`, cancels
  the transfer (`TransferCancel{BAD_REQUEST}`). Disconnect cancels too. No
  resume for uploads.
- Window: 16 KB on current firmware. The device acks every 4 KB and on
  `UploadEnd`. A stall of 20 s without data cancels with TIMEOUT.

**Download** (device → phone):

```
DownloadRequest{xfer, source, name, offset, window}  →
                                                    ←  DownloadBegin{xfer, size, offset}  (or Result: NOT_FOUND, RECORDING, NO_SD …)
                                                    ←  Chunk{xfer, offset, data} …   while sent − acked < window
TransferAck{xfer, received}  at least every window/2 →
                                                    ←  TransferDone{xfer, OK, size, crc32 of the whole file}
```

- Window default 8 KB. The phone acks every 2 KB. No ack for 10 s cancels with
  TIMEOUT. `offset` lets the phone resume an interrupted ride download.
  `DownloadBegin.offset` is the first byte the device will send. 0 in answer
  to a resume means "starting over", and the phone drops what it had. The
  CRC in `TransferDone` covers the whole file, so the phone hashes what it
  already had plus what arrived.
- Rides and logs refuse with RECORDING while a ride records, as before.

**Route upload** keeps the legacy steps: `UploadBegin{TARGET_ROUTE, name}`
(clears pending maneuvers) → chunks → `UploadEnd` → `TransferDone` (parse
result). Then `RouteManeuvers` pages (≤16 each, ≤ `DeviceHello.max_maneuvers`
in total) → `RouteFinish` → `Result`, which raises "Start navigation?".

**Album art**: `UploadBegin{TARGET_ALBUM_ART, art_format, art_width, art_height}`
with `size` = w·h (GRAY8) or ⌈w·h/3⌉ (TONE5). It is dropped if the title
changes before it commits.

**Tiles**: `name` is the H3 cell id; POI files use `TARGET_MAP_POI`. The
version record (`cdn:<hex>`, `cdn:`, `phone:<secs>`) goes in `tile_version`.

**Starred segments** (FEATURE_SEGMENTS): `UploadBegin{TARGET_SEGMENTS}` with
the whole set as one blob in segment_match.h's format (`"OTSG"`, version,
count, then each segment), at most `seg::BLOB_MAX` bytes. The blob version
must not be newer than `DeviceHello.segments_version` (2 = with elevation).
The device validates the blob as a whole, keeps it on the card
(`/segments/starred.bin`) and hands it to the matcher. A rejected blob gets
`TransferDone{PARSE_ERROR, detail}` and the previous set stays. Either way a
`SegmentsInfo{present, count, version}` push (`req = 0`) comes just before the
`TransferDone`. `GetSegments` asks for the same `SegmentsInfo` at
any time (the app's "N on the device"). The legacy route characteristic's
0x09..0x0C ops stay for apps without the link.

## Lists

`ListRequest{kind}` answers with pages carrying `last = true` on the final one.
An empty list is one page. Rides and logs carry sizes. `LIST_MAP_TILES` and
`LIST_MAP_POIS` answer with `TileList` (with versions when the device has
FEATURE_MAP_TILE_VERSIONS). `LIST_MAPS` answers with one `MapCoverageList`.

## Firmware structure (src/link.cpp)

- **RX** (`onWrite`, NimBLE host task): reassemble into a PSRAM buffer, then
  copy the complete message into a ring queue. Nothing else happens on the
  host task. This closes the legacy exceptions: GPX parsing and notifying
  from `onWrite`.
- **Link task** ("link", its own stack, sized for nanopb plus SD work):
  decodes `PhoneMsg` into a static struct, dispatches, writes upload chunks
  into the PSRAM staging buffers, streams downloads, and owns the only
  `notify()` on TX through the two-queue sender. Commits that need the SD card
  wait for `sdFree` exactly like the legacy path. The srv task's 1 Hz work
  (live frames, workout status, sensor pushes) builds messages and enqueues
  them. It does not send.
- Feature code is shared with the legacy characteristics. Each feature has one
  function that does the work (`routes::loadFromMemory`, `mapCommit`, …).
  Legacy and link differ only in how the request arrives and how the answer is
  encoded.
- `linkActive` (this connection said `Hello`) routes every push to the link
  and stops the legacy notifications.

## Apps

- **iOS**: `LinkClient` (framing, reassembly, request ids with completion
  handlers, two-queue writer) plus `Link.pb.swift` (generated, swift-protobuf).
  `BLEManager` uses the link for every feature when `link.ready`, and its
  existing legacy code otherwise.
- **Android**: `link/LinkClient.kt` on top of `GattQueue`, generated protobuf-lite
  classes from `proto/link.proto` (gradle protobuf plugin), the same
  link-or-legacy switch in `BleManager`. Android gets the live Ride view
  through the link.

## Regenerating

`tools/gen_proto.sh` regenerates the firmware (nanopb 0.4.9.1) and iOS
(protoc-gen-swift) code that is committed. Android generates at build time.
After editing `link.proto`, update `link.options` for any new string, bytes or
repeated field. Without a limit nanopb falls back to callbacks and the
firmware won't compile against them.
