# Play Store listing assets

What is uploaded to the Play Console default (en-US) store listing, so the
next release can refresh the listing without rebuilding it from scratch.
Text lives in `../../companion-ios/appstore/METADATA.md` (shared with iOS;
Play's short description is "Routes, offline maps and rides for the
OpenTrailPaper e-paper bike computer", full description = the METADATA
description plus a MESH paragraph).

| File | Slot | Spec |
|---|---|---|
| `icon-512.png` | App icon | 512 × 512, the iOS 1024 icon downscaled |
| `feature-graphic.png` | Feature graphic | 1024 × 500 |
| `phone-01..08.png` | Phone screenshots | 1080 × 1920 (9:16) |
| `tablet-01..02.png` | 7-inch and 10-inch tablet screenshots | 1920 × 1080 (16:9), same two files in both slots |

These are rendered from the Claude Design file `play-store-screenshots.dc.html`
(project "E-ink bike GPS screen", file "Play Store Screenshots"), which frames
the raw captures below with headlines. To re-render after changing the design
or the captures: pull the `.dc.html`, strip the `data-omelette-injected`
blocks, extract each `[data-shot]` element into its own page with the Google
Fonts link, and screenshot it with headless Chrome
(`--force-device-scale-factor=2 --window-size=540,960` for phones,
`960,540` for tablets, scale 1 at `1024,500` for the feature graphic; one
`--user-data-dir` per run). Listing uploaded 2026-10-01.

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
