# Varia radar and vertical tiles

Update the head unit and companion app from this branch. In **Sensors** on the
head unit or phone, scan, put the Varia into Bluetooth pairing mode, and choose
**Connect**. The head unit remembers the radar's address and reconnects during
rides, just as it does for heart-rate and power sensors. **Forget** removes the
pairing. The phone is not needed after pairing.

In the phone's dashboard editor, add **Radar** to a data page and save it to the
head unit. Choose **Height**: **Half**, **Three-quarter**, or **Full** (the default).
For shorter tiles, choose **Alignment: Top or Bottom** in the radar row. Full-height
tiles fill the page in either position. Existing layouts default to Top.
The radar sits on the right and fits whole grid rows. Bottom placement chooses
the closest readable row boundary, so a large hero above it can remain full-width. Its bottom border aligns with the last adjacent tile, with
only the normal gutter before the next row. This also applies during navigation;
with just one numeric row, all height choices fill that row. Fields beside it
pack on the left; rows starting below the tile expand across the page. Adding
Radar automatically arranges the other fields; they have only the usual size and
half-width controls. Radar has its own height and alignment controls in its editor row. Only one
radar tile is allowed per page. A page containing only radar shows speed beside it.

The traffic view follows the supplied Radar View.html reference: the rider is
at the top, traffic moves upward as distance decreases, and rectangular markers
use outline (>100 m), dotted (40–100 m), and solid (<40 m) ink. The header shows
the track count and changes to CAR BACK when the nearest target is under 40 m.
Each marker stays at the reported distance. Crowded distance labels are omitted,
nearest first, so a label never overwrites another car. Metric distances use
metres; standard (imperial) distances use feet. The 150 m / 492 ft scale saturates at its
far end (marked +); the number still shows the reported distance beyond it.

A configured radar tile stays visible even with Hide offline fields enabled.
OFFLINE means no link, NO SIGNAL means no fresh measurements, and ALL CLEAR
requires a live stream with no remaining tracks. Tracks expire after two seconds;
a silent stream becomes NO SIGNAL by that same deadline, rather than appearing
clear. After five seconds of silence the BLE task reconnects. The UI samples at
1 Hz and uses the existing display delta engine; the 160 px tile begins at x=352,
with both edges aligned to eight pixels. The layout stays fixed on disconnect.

## Compatibility

This is an independent implementation of the **legacy Varia BLE radar stream**:
service `6a4e3200-667b-11e3-949a-0800200c9a66`, notify characteristic
`6a4e3203-667b-11e3-949a-0800200c9a66`. Discovery also recognizes Garmin member
service `FE1F` with a Varia model name; the actual radar characteristic is checked
before connecting. A Garmin watch advertising FE1F alone is not classified as a
radar. Existing sensor-mask bits are unchanged; radar uses bit 3 (`0x08`).

The intended family is BLE-equipped Varia RVR315 / RTL515 / RTL516 / RCT715 and
other models exposing that legacy characteristic. RearVue 820's legacy stream
is documented by community captures, but its newer encrypted V2 stream, lateral
position and vehicle classification are not implemented. ANT-only models such
as RTL510 require an ANT radio and are not supported by this BLE integration.
No physical radar was available for validation in this change; model-specific
compatibility still needs confirmation on hardware.

The third byte of a legacy target record has conflicting speed interpretations
across public clients and newer models. It is deliberately not used for speed
or collision-time estimates. The feature is a visual traffic tile; it does not
change the radar's light settings or add audio alerts.

## Config and implementation

The dashboard text format supports `vertical` alongside `half`, plus optional
`height=50`, `height=75`, or `height=100` for vertical tiles. Add `position=bottom`
to anchor radar to the final grid rows; omitted or `position=top` keeps it at the top:

```
speed      large
power3s    medium
hr         medium
ridetime   medium
radar      medium vertical height=50 position=bottom
page map
```

Missing or unsupported heights default to full height, preserving older configs.
Height is ignored for ordinary rows. Short radar tiles compress the distance lane while retaining the full
150 m scale and count; overlapping labels favor the nearest vehicle.

Radar is always vertical, including a bare `radar` line. Legacy numeric vertical
fields return to ordinary full-width rows; duplicate radar fields are discarded. Both app parsers mirror firmware normalization.
Radar is excluded from the map's short numeric strip. Existing field IDs and
sensor kinds retain their original values. Pairings use separate NVS keys
`sens_rdr` and `snm_rdr`, preserving all previous pairings.

`radar.h/.cpp` hold a bounded, host-testable parser and state. BLE callbacks and
the expiry sweep mutate it under the shared ride-state lock; the renderer uses
a copied snapshot. At most eight nearest tracks are retained. Heartbeats refresh
stream liveness, per-track ages preserve targets across fragmented packets, and
malformed/unknown frames cannot refresh the stream. Sequence continuation checks
and unsigned timestamps handle fragmented messages and millis rollover.

## Verification

- `sh tools/radar_test/run.sh`: real C++ parser/layout under AddressSanitizer and
  UBSan, plus the Swift layout parser/serializer. Covers truncation, unknown
  frames, sentinels, fragments, heartbeat-only clearing, disconnects, stale
  streams, rollover, target bounds, config round trips and legacy configs.
- Android `:app:testDebugUnitTest` includes matching `RadarLayoutTest` cases.
- `sh tools/preview/render_preview.sh`: real renderer previews for traffic,
  clear, no signal, offline, imperial and eight crowded targets with navigation,
  plus half/three-quarter tiles, bottom alignment, navigation range endpoints,
  and a pixel check that hero power indicators stay out of the radar gutter.
- Both companion app builds and `pio run -e t5s3-painter` validate integration.
- Hardware follow-up: pair the actual Varia, verify recorded packet distances,
  drive targets in demo mode, turn the radar off/on, check simultaneous HR/power/
  cadence/phone links, and inspect refresh behavior on the e-paper panel.

Protocol references (used for interoperability, not copied decoder code):
[legacy RVR315 client](https://zacharybull.com/pycycling/_modules/pycycling/rear_view_radar.html),
[legacy/820 packet notes](https://github.com/partymola/bike-radar-docs/blob/main/PROTOCOL.md),
[RTL515 service discussion](https://forums.garmin.com/developer/connect-iq/f/discussion/240452/bluetooth-profile-for-garmin-varia-rtl515/1224404?pifragment-1298=1).
