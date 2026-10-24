# App changelog

Rider-facing notes per iOS build, same convention as the firmware's
CHANGELOG.md: newest first, one `## <version> (build N)` heading each,
written for the rider. The current version's section is the source for the
App Store "What's New" text — paste it (minus build-only entries) when
submitting.

## Unreleased

- Route: with Strava or RideWithGPS connected, import one of your saved
  routes (iOS: the download button on the Route map; Android: the file
  button). Pick it from the list, which shows each route's distance and
  climbing, and send it to the device like a GPX file. To see private
  Strava routes, disconnect and connect Strava again in Settings >
  Accounts; until you do, only public ones are listed.
- Dashboard editor: Add a page > Stopwatch puts the device's new STOPWATCH
  page in the Home-key cycle. It needs firmware v1.23. Older firmware
  leaves the page out, and the editor shows the layout the device kept.
  The Android app has the same option.

## 1.3 (build 17)

Version numbers now follow the App Store: 0.6 (build 15) shipped there as
1.2. The Android app carries the same version.

- Bluetooth speed sensors: pair one in Sensors and the device uses it for
  speed and distance. Set the wheel size in Settings by choosing the rim
  and tyre width; the app works out the circumference.
- Device settings can be changed while the device is away; they are sent
  the next time it connects. If the same setting was changed on the device
  in the meantime, the app says so.
- Pairing: the app pairs with one device using the code on the panel, and
  explains when the device is already paired with another phone instead
  of retrying.
- Maps: hexes sent from another phone or before a reinstall no longer
  show "update available" on firmware v1.22.
- The Ride tab fits on one screen: speed beside heart rate and power, with
  dashboard and workouts as full-width rows.
- Average speed is available as a dashboard field.
- Long map and route transfers keep going in the background, with a Live
  Activity showing progress.
- Phone GPS sends speed and heading to the device.
- Mesh: nodes by short name, hop counts on the node map.
- Controls stay readable when the phone is in dark mode.
- Strava uploads retry after an expired sign-in and show readable errors.

## 0.6 (build 15)

- Album art on the MUSIC page: covers from the Music app (allow access in
  the dashboard editor's music page), and — with "Look up album art online"
  turned on — from Apple's iTunes catalogue for Spotify and other players.
  Covers go over pre-dithered, a third of the bytes, on firmware that
  supports it.
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
