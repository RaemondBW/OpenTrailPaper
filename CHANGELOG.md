# Firmware changelog

Rider-facing notes per firmware version. CI slices the section matching the
released version into the GitHub release body (see .github/workflows/build.yml),
and the app shows it under Settings → Firmware when an update is available —
so write for the rider, newest version first, one `## vX.YY` heading each.

## v1.23

- Roads through parks are easier to follow on the map. Where a road or
  cycleway crosses a park's striped fill it now has a thin white border, so
  it reads as a clean line instead of disappearing into the stripes.
- The GPS debug page no longer shows more satellites in use than in view.
  The in-view count is refreshed every five seconds and the in-use count
  every second, so for a moment after the sky changed (or right after the
  GPS woke) the older number could be the smaller one.
- The GPS module now talks to the device at 115200 baud instead of 9600, so
  each second's position update takes about 25 ms on the wire instead of
  300 ms and the device can sleep for the difference. The module is found
  at whatever rate it was left at, moved to 115200 and keeps that setting,
  so a GPS that came from another firmware at an odd rate still gets a fix.
- New optional STOPWATCH page, off until you add it from the app's
  dashboard editor (Add a page > Stopwatch). It shows big digits with three
  buttons along the bottom: START/PAUSE, RESET and LAP. LAP works while the
  clock runs. RESET works only once it is paused, so a stray tap can't wipe
  it mid-effort. Laps list newest first, each with its time, average speed,
  and average cadence and power when you have those sensors. The lap in
  progress shows its averages live. Speed is in your km/h or mph setting
  and is shown as "--" with no GPS or speed sensor. Power counts zeros
  while coasting. Cadence leaves out coasting at 0 rpm. Paused time never
  counts. The RPM and WATTS columns only appear when a cadence source or
  power meter is reporting, or a lap has its data. With fewer columns the
  rest get more room. A lap with no data for a column shows "--". Up to 99 laps are kept. It is separate from the ride timer and
  keeps running while you're on other pages. While it runs, the device
  won't switch itself off for being idle.
- The device now says when it has no usable SD card. A "NO SD" chip shows
  in the status bar ("SD ERR" for a card it can't read), the boot screen
  says "No SD card", Start Ride in the menu explains it can't record, and
  the map's "no map here" box tells you to insert a card. A card that is
  present but won't mount is almost always exFAT, which the device can't
  read, so those messages ask for FAT32. Put a card in while it's on and
  it is picked up within 30 seconds and the warnings clear. The companion
  apps get the same status and show it too.
- New optional SEGMENT page for live Strava segments, off until you add it
  from the app's dashboard editor (Add a page > Segment). Star segments on
  Strava, then sync them from the app (Settings > Accounts > Strava). Up to
  50 are kept on the device. The page draws the segment's line (what
  you've ridden thick, the rest dashed, you as a dot) and its elevation
  profile (ridden part solid, the rest hatched). Away from them, it shows
  the nearest starred segment within 5 km and how far its start is. Ride through a
  start in the segment's direction and the device times the effort: the
  time, how far ahead of or behind your PR pace you are, and a progress bar
  with the distance to go. At the end it shows your time against your PR,
  or NEW PR, for 30 seconds. Leave the segment for more than 20 seconds and
  the effort is dropped. Efforts are timed whatever page you're on. The page
  switches itself on when an effort starts and goes back afterwards; the
  dashboard editor can turn that off. The device's times come from its own
  GPS and can differ from Strava's by a second or two.

## v1.22

- Fixes the mid-ride restart. A stray interrupt from the display power
  chip's I2C bus could trap the device in an endless loop until the
  watchdog reset it; the bus driver now clears that interrupt and carries on.
  Any bus traffic also keeps the device awake until it finishes.
- The Menu responds to taps again on a card with lots of rides. It was
  re-counting every ride on the SD card once a second, which with a few
  dozen rides kept it busy for most of that second.
- Navigate and Ride History now page through everything on the card, with
  the same up/down arrows and "1-5 / 72" counter as the workout list.
  Ride History used to stop after five rides; it now lists them all,
  newest first. Both read the card once when you open them (REFRESH
  re-reads it), so paging is instant.
- The device now remembers which version of each map hex it holds. The
  app's Maps screen no longer marks a hex as "update available" just
  because it was sent from another phone or before an app reinstall.
  Hexes sent before this update keep the old behaviour until they are
  sent again.
- Phone pairing is stricter and quieter. Everything the app reads or writes
  now needs a passkey pairing (the code on the panel). While the device is
  paired, any other phone is turned away before it can be asked for a code.
  That includes your own phone if it forgot the device. Before, an iPhone
  could get stuck asking for a code over and over while the panel showed
  none. The app now says what happened and stops retrying. To pair again,
  go to Paired Devices > Phone > Unpair on the device, then connect.
- The pairing code now also shows over the "Start navigation?" prompt.
- A phone reconnecting stays connected while it finishes pairing. Before,
  the device could doze off during the handshake and drop it.
- Bluetooth speed sensors: pair one in the app's Sensors screen and it
  becomes the speed source, ahead of GPS, so speed and distance keep
  counting under trees and in tunnels. Set your wheel size in the app's
  Settings by picking the rim and tyre width.
- New dashboard field: average speed (distance over moving time).
- The WORKOUT page lists the workouts on the card when none is loaded, so
  you can start one from the device.
- Speed drops to zero when the GPS fix is lost or goes stale, instead of
  holding the last value. With a phone connected, speed and heading can
  also come from the phone's location.
- Mesh: nodes are labelled by their short name, the node map shows how
  many hops away each one is, and direct messages are encrypted to the
  other node's public key.
- The frontlight works again after a power-off, and the status bar is
  greyed under the power-off overlay too.
- Crash reports are saved to the card as soon as it mounts, before it is
  shared over USB.

## v1.21

- Fixes a reset that could happen mid-ride. When the screen refreshed at
  the same moment the device dropped into its power-saving sleep, the
  device could lock up and restart.
- The frontlight is now held fully off while the device is powered off.
- Battery drain while powered off is still being tracked down. This
  version leaves the screen, GPS and mesh radio connections in a clean
  state for power-off, and powering off takes several seconds longer for
  now: the device measures its own power draw on the way down so the
  remaining drain can be found.

## v1.20

- Garmin Varia radar support. Pair a Varia from the app or the Sensors
  screen and add a radar tile to any dashboard page: a column down the
  right side that shows cars coming up behind you and how far back they
  are, in feet or metres to match your units. You can make it full, three-
  quarter or half height, and pin it to the top or bottom of the page.
- Longer battery life. The device now sleeps between GPS updates and
  while the phone and sensors are connected but quiet, instead of staying
  fully awake the whole time it is on.
- Mesh messaging works outside the US. Choose your LoRa region, transmit
  power and frequency slot from the app's Mesh settings instead of
  rebuilding the firmware. Existing devices stay on the US band until you
  change it.
- Rides uploaded to Strava, intervals.icu and RideWithGPS now show
  OpenTrailPaper as the recording device, with the firmware version.
- If the device crashes, it saves a crash report to the SD card on the
  next boot, so a problem can be tracked down after the ride.

## v1.19

- A device with no paired sensors can find its first one again. Scanning
  from the app or the Sensors screen did nothing on a brand-new or
  factory-reset unit, so nothing could ever be paired. Devices that already
  had sensors were unaffected.

## v1.18

- New or factory-reset devices start with the frontlight off. E-paper needs
  no light in daylight, and the light was burning battery on units nobody
  had asked to light up. Riders who set a level keep it.
- Dashboard fields for sensors that aren't connected stay on the panel as
  "no data" by default, so the layout you configured is the layout you see
  and a missing strap or power meter is obvious. Riders who turned this off
  keep that choice.

## v1.17

- If the device resets or runs out of battery mid-ride, the next boot asks
  what to do with the interrupted ride: continue recording right where it
  left off, save it as a complete ride, or discard it. Distance, time and
  averages carry over on continue.
- Rides interrupted long ago are also found now, even with a full card —
  recovery used to stop looking after the 32 oldest files.
- Stopping a ride answers instantly: SAVE and DISCARD show what they're
  doing while the ride is written to the card, and powering off shows
  POWERING OFF right away instead of sitting still for a few seconds.
- The powered-off screen always finishes drawing — it could freeze
  half-written — and now shows the OpenTrailPaper mark above the map.
- New typefaces, drawn on the device at exactly the size each field needs.
  They look like the old ones on purpose, but they're open fonts, they're
  sharper at every size, and the firmware is about 1 MB smaller.
- The device now remembers several paired sensors of the same kind — both
  bikes' power meters can stay paired, and whichever wakes up connects.
- Turn directions pop up on the music and workout pages too, instead of
  only on pages that had a spot reserved for them.
- Map and tile saves retry automatically on a card hiccup, and log enough
  to tell what actually failed — a phone re-send was the slow fallback.

## v1.16

- Workouts auto-pause when you stop: five seconds with no power and no
  movement holds the workout clock, and it resumes the moment you pedal
  or roll again.
- The auto-pause banner no longer covers the music or workout pages —
  those pages keep their content and show the pause in the top bar.
- Cleaner workout page: everything about the current block lives in one
  dark panel, with the zone and block length under the target.
- The music page top bar shows sensor and pause status instead of a title.
- No more ghost of the pause banner after resuming.

## v1.15

- Structured workouts: ride .erg/.mrc workouts with a live target, block
  countdown and session profile on a new dashboard page. Build, edit and send
  workouts from the app, or drop files in /workouts on the SD card.
- Optional "pause after every block" mode holds the workout at each boundary
  until you're ready.
- Music page: see and control what's playing on your phone, with album art.
- Dashboard pages: arrange multiple data pages, music, workout and the map
  into a carousel the Home key cycles; the app's editor rearranges them.
- Sensors now stay connected through pauses and quiet stretches — fixed the
  radio sleep bug that dropped heart rate and power every few seconds after
  a stop.
- Ride timer auto-pauses at stops and resumes when you roll (or by tapping
  the banner); long stops let the device sleep and it wakes fully on resume.
- Sharper page switches: the screen scrubs ghosting between dashboard pages.
- Ride files are named in your local time, not UTC.
- Lower idle battery drain.

## v1.14

- Fixed a silent boot freeze with an SD card inserted.
- Meshtastic: text messaging with nearby nodes over LoRa, with private
  channels shareable from the app.
- Flashing from the app or site no longer needs the BOOT/RESET buttons.
- Faster map tile loading.

## v1.13

- Ride and log downloads to the app no longer conflict mid-transfer.
- Panel power rails shut down cleanly before deep sleep.
