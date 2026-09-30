# App Store metadata

Copy-paste-ready text for App Store Connect. Character limits noted per field.
Promotional text and keywords can be changed any time **without** submitting a
new build; the description updates with a new version.

---

## Subtitle (≤30 chars)

`Maps & routes for your ride` — 27

Alternates:
- `Your e-paper bike computer` — 26
- `Offline maps for cycling` — 24

---

## Promotional text (≤170 chars)

> New: send rides to Strava, RideWithGPS or Intervals.icu with a tap, pair a Garmin Varia radar, and lay out your dashboard right on the panel preview.

(149 chars, 0.6 launch)

Alternate (evergreen):
> Turn your DIY e-paper head unit into a full bike computer — plan routes, bake offline maps, and sync rides over Bluetooth. No account required, no subscription.

(160 chars)

Alternate:
> Plan routes, build offline maps, and review every ride, then send it all to your e-paper bike computer over Bluetooth. Works fully offline once set up.

(150 chars)

---

## Keywords (≤100 chars, comma-separated, no spaces)

`cycling,bike,computer,gps,navigation,offline,maps,gpx,route,ride,tracker,eink,gravel,mtb,cyclometer`

(99 chars. Single words separated by commas — the App Store auto-combines them
into phrases like "bike computer" and "offline maps", so there's no need to
spend characters on multi-word terms. Don't repeat words already in the app
name "OpenTrailPaper".)

---

## Description (≤4000 chars)

OpenTrailPaper is the free companion app for the OpenTrailPaper e-paper bike computer — an open-source, sunlight-readable head unit you build yourself. The app handles everything that's awkward on a touch e-paper screen: planning routes, building offline maps, and reviewing your rides. Then it sends it all to your head unit over Bluetooth. No account required, no subscription.

PLAN ROUTES
Search for any destination, preview the route on the map, and send it to your device as a GPX file. Turn-by-turn prompts guide you along the way — and your head unit follows the route even with no SD card and no signal.

OFFLINE MAPS
Pick the area you ride and the app bakes offline map tiles straight onto the device's SD card. Roads, water, and parks render on the e-paper display wherever you go — no data connection required, ever.

REVIEW YOUR RIDES
Pull your recorded rides off the device and see distance, moving time, speed, elevation gain, power, and heart rate, with your track drawn on the map. Every ride is a standard .fit file you can export and open anywhere.

SHARE YOUR RIDES (OPTIONAL)
Connect Strava, RideWithGPS or Intervals.icu and upload any ride with a tap. Sign-in runs through OpenTrailPaper's own sync service, so the app never holds a provider secret, and rides only leave your phone when you choose to send them.

TUNE YOUR DEVICE
Adjust FTP, time zone, units, and backlight, pair Bluetooth heart-rate and power sensors and a Garmin Varia radar, and arrange the dashboard pages on a live preview of the panel — all from your phone. Settings are saved to the device's flash so they persist across reboots. When a new firmware build is ready, update over the air with one tap.

BUILT TO STAY OUT OF THE WAY
- Works completely offline once set up — no phone or signal needed on the bike
- No account and no subscription, ever
- Nothing leaves your devices unless you choose to send a ride to a connected account
- Fully open source

WHY LOCATION AND BLUETOOTH
Bluetooth connects the app to your head unit to sync routes, maps, settings, and rides. Location (only while you're using the app) shows your position on the map, warm-starts the device's GPS so it locks on faster, and acts as a backup fix when the device can't see the sky.

OpenTrailPaper pairs with the DIY OpenTrailPaper head unit. Build instructions, firmware, and full source are on GitHub.

(~2,375 chars)

---

## What's New (≤4000 chars) — 0.6 (build 15)

Source: the current section of `../CHANGELOG.md`, expanded for the store.

ACCOUNTS
Connect Strava, RideWithGPS and Intervals.icu in Settings, then upload any ride from its detail view with a tap. Sign-in happens in your browser through OpenTrailPaper's own sync service, so the app never holds a provider password or secret, and Strava shows the activity as recorded on an OpenTrailPaper. Entirely optional: rides stay on your own devices unless you choose to send one.

GARMIN VARIA RADAR
Pair a Varia in Sensors and add a radar tile to any dashboard page. Set its height and whether it sits at the top or bottom of the page. Distances follow your unit setting.

DASHBOARD EDITOR
Edit a data page on the panel itself. Tap a cell to select it, hold and drag to move it, drop it onto half of a wide cell to pair the two, and tap the size badge to resize. What you arrange is exactly what the device draws.

MESH RADIO
Choose the LoRa region, transmit power and frequency slot from the Radio section of Mesh settings. Riders outside the US no longer need a custom firmware build.

ALSO
- The Apple Music companion integration is gone; media controls go straight through the device's Bluetooth media link.
- Ride uploads to Intervals.icu no longer need a per-user API key.

(1195 chars)
