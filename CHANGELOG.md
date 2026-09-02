# Firmware changelog

Rider-facing notes per firmware version. CI slices the section matching the
released version into the GitHub release body (see .github/workflows/build.yml),
and the app shows it under Settings → Firmware when an update is available —
so write for the rider, newest version first, one `## vX.YY` heading each.

## Unreleased

- Android: import a `.gpx` route — pick one in the app or open it from Files,
  Komoot or Strava. The route is previewed, sent to the head unit exactly as it
  came, and can optionally be given turn cues derived from OSRM.
- Android: route names and turn cues are now trimmed to the firmware's byte
  budgets on codepoint boundaries, so a long non-ASCII street name can no longer
  put a half character on the panel.

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
