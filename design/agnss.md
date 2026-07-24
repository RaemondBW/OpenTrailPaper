# AGNSS (assisted GPS) — ephemeris injection

Goal: cut time-to-first-fix, and let the receiver fix in weaker signal, by
**handing it the satellite ephemeris** instead of making it demodulate the
50 bps navigation message off the air (which needs ~28–31 dB-Hz). With
ephemeris + coarse time + position pre-loaded, the receiver only has to *track*
(measure pseudorange), which works down to ~24 dB-Hz.

This builds directly on the aiding we already ship (`gps_service::injectAiding`,
CASIC `AID-INI` / u-blox `MGA-INI`): AGNSS adds the ephemeris on top of the
position+time seed.

## The receiver is the parser (firmware is a dumb pipe)

The ephemeris blob is **module-specific** and we do NOT parse it on the device:

- **u-blox** (MIA-M10Q variant): a stream of `UBX-MGA-*` messages (AssistNow
  Online = current ephemeris valid hours; AssistNow Offline / `MGA-ANO` =
  predicted, valid days–weeks).
- **CASIC** (L76K variant): the vendor AGNSS binary — a stream of CASIC
  messages.

In both cases the phone fetches the right blob and the firmware just streams the
bytes to the GPS UART; the module ingests its own format. So the firmware needs
no per-format knowledge — only a paced byte pipe and a module-type readout so
the phone knows which blob to fetch.

## BLE protocol — CHR_AGNSS (`b1c50008-…`)

Opcode in byte 0, mirroring the map/OTA characteristics.

Phone → device (write):
- `0x10` query        → device notifies `{0x10, moduleKind, hasFix}`
                         (moduleKind: 0 none, 1 CASIC, 2 u-blox)
- `0x01` begin        → reset the inject stream; device re-seeds AID-INI
- `0x02` <bytes…>     → append raw AGNSS bytes to the inject stream
- `0x03` end          → device re-seeds AID-INI (bounds ephemeris to "now")
- `0x04` abort        → drop the inject stream

Device → phone (notify): `{0x10,…}` query reply, `0xA1` begin-ack,
`0xA3` done, `0xAF` error.

Position + precise-ish time already flow over CHR_ROUTE's seed path
(`seedPosition`), so the app should send a fresh location right before AGNSS.

## Firmware plumbing (this branch)

- `gps_service`: a FreeRTOS **stream buffer** (SPSC, thread-safe) fed by the BLE
  task and drained on the GPS task into `SerialGPS`, paced by
  `availableForWrite()` so a multi-KB blob at 9600/38400 baud never stalls NMEA
  parsing. API: `agnssBegin()`, `agnssInject(bytes,len)`, `agnssEnd()`,
  plus `moduleKindCode()` for the query reply.
- `ble_server`: CHR_AGNSS + `AgnssCb` implementing the opcodes above.

Streaming with no per-message ACK is fine for a first cut; u-blox MGA supports
`MGA-ACK` flow control we can add later if injection proves lossy at speed.

## Phone side (next, separate codebase) — the real work

1. Read module type via the `0x10` query.
2. Fetch an AGNSS blob for that module:
   - u-blox → AssistNow (needs a free Thingstream token) — Online for a quick
     fix, Offline for multi-day validity when away from signal.
   - CASIC → the vendor AGNSS endpoint.
   **Open dependency:** both want an account/token or a vendor URL; picking the
   data source (and caching Offline data on the phone) is the main app task.
3. Send a fresh position/time (existing seed path), then `begin` → stream the
   blob in MTU-sized `0x02` chunks → `end`.
4. Refresh Offline data every few days; skip injection when the device already
   reports a fix.

## Validation

- Firmware pipe: a console command streams a canned `AID-INI` through the same
  path to prove BLE→UART plumbing without a data source.
- End-to-end: with real ephemeris, compare `cold` vs `cold`+AGNSS TTFF via the
  serial telemetry, especially in weak signal (the `snr≈17`, 0-sats-used case).
