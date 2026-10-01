# Play Store listing assets

What is uploaded to the Play Console default (en-US) store listing, so the
next release can refresh the listing without rebuilding it from scratch.
Text lives in `../../companion-ios/appstore/METADATA.md` (shared with iOS;
Play's short description is "Routes, offline maps and rides for the
OpenTrailPaper e-paper bike computer", full description = the METADATA
description plus a MESH paragraph).

| File | Slot | Spec |
|---|---|---|
| `icon-512.png` | App icon | 512 × 512, the iOS 1024 icon downscaled on white |
| `feature-graphic-1024x500.png` | Feature graphic | 1024 × 500, icon + name on the paper colour |
| `shot-0N-*.png` | Phone, 7-inch and 10-inch tablet screenshots | 1350 × 2400 (exact 9:16) |

Screenshots are emulator captures (`scanner` AVD, Pixel 7 profile, 1080 × 2400)
padded to 9:16 with the paper colour `#F2F0E8`; Play rejects anything taller
than 2:1. The tablet slots reuse the phone set — replace with real tablet
captures when the layout gets tablet treatment.

Raw 1080 × 2400 captures live in `raw/` (01–15 the plain app, 16–25 the
seeded states below).

Demo launch extras (debug or release build, no head unit needed), the Android
twin of the iOS `-demo-*` arguments:

| Extra | Effect |
|---|---|
| `--ez demo-connected true` | looks paired: "OpenTrailPaper" chip, firmware row, Send to device enabled |
| `--ez demo-rides true` | three cached rides decoded from `assets/demo.fit` |
| `--ez demo-route true` | Route tab builds Golden Gate Park → Fisherman's Wharf via OSRM (needs network) |
| `--ez demo-accounts true` | Strava and Intervals.icu show as connected (in memory only) |
| `--ez demo-dash true` | device-default dashboard config for the editor |

Recipe (emulator booted, debug build installed, animations off):

```sh
ADB=$ANDROID_HOME/platform-tools/adb
$ADB shell am start -n com.raemond.opentrailpaper/.ui.MainActivity   # first run → tutorial
$ADB exec-out screencap -p > raw-00-first.png                          # Welcome
$ADB shell input tap 540 2190; $ADB exec-out screencap -p > raw-tut-1.png   # What you'll do
# finish the tutorial, then tabs sit at y=2240: Ride 100, Route 320, Rides 540, Mesh 760, Settings 980
$ADB shell am start -n com.raemond.opentrailpaper/.ui.MainActivity --ez demo-dash true  # populated dashboard
$ADB shell am start -n com.raemond.opentrailpaper/.ui.MainActivity \\
  --ez demo-connected true --ez demo-rides true --ez demo-route true --ez demo-accounts true
# Workout builder: Ride tab → Workouts card (540,1504) → Create a workout (300,819); it opens with a 5-block session.
# Tabs want ~9 s after launch before the first tap registers on the emulator.
```

Then pad: paste each 1080 × 2400 capture at x=135 on a 1350 × 2400 `#F2F0E8` canvas.
