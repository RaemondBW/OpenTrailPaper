# App changelog

Rider-facing notes per iOS build, same convention as the firmware's
CHANGELOG.md: newest first, one `## <version> (build N)` heading each,
written for the rider. The current version's section is the source for the
App Store "What's New" text — paste it (minus build-only entries) when
submitting.

## 0.6 (build 14)

- Accounts: connect Strava, RideWithGPS and Intervals.icu in Settings and
  upload any ride from its detail view. Sign-in runs through
  sync.opentrailpaper.com; the app never holds a provider secret, and
  Strava shows the activity as recorded on an OpenTrailPaper.
- Garmin Varia radar: pair a radar in Sensors and add a radar tile to a
  dashboard page, with adjustable height and top/bottom alignment.
  Distances follow your unit setting.
- Dashboard editor: edit a data page on the panel itself — tap a cell to
  select it, hold and drag to move it, drop onto half of a wide cell to
  pair them, tap the size badge to resize.
- Mesh: choose the LoRa region, TX power and frequency slot from the
  Radio section of Mesh settings.
- Music: the Apple Music companion integration is gone; media controls go
  straight through the device's Bluetooth media link.

## 0.5 (build 13)

- The firmware "What's new" panel starts collapsed with a preview of the
  top change; tap to expand.

## 0.5 (build 12)

- Workouts: build interval workouts block by block (zones scale from the
  device's FTP), import .erg/.mrc files, and edit anything — including
  workouts already on the device. The phone keeps every workout; send any
  of them to the device with a tap.
- Live workout session card: block, target, progress and countdown, with
  start/pause/skip/stop and a "pause after every block" toggle.
- With no workout loaded, the Workouts screen is a one-tap picker.
- Dashboard editor gained the workout page, alongside data pages, music
  and the map.
- Firmware updates show their release notes before you install.
- Ride names display exactly as the device records them (local time) —
  they no longer shifted by a timezone.
- Mesh: private channels can be created and shared from the app.

## 0.4 (build 11)

- Dashboard editor: arrange the device's pages — data pages and music —
  with drag-and-drop, live previews, and per-field sizes.
- Music page support and media pairing.
- Meshtastic: chat with nearby nodes, node list and map, channel setup.
- Ride and log transfers queue instead of conflicting mid-download.
- The tutorial drives a live rendering of the head unit.

## 0.3

- First App Store release: live ride view, route planning and GPX upload
  with turn cues, ride downloads with FIT export, map area downloads,
  sensor pairing, firmware updates over Bluetooth.
