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

Recipe (emulator booted, debug build installed, animations off):

```sh
ADB=$ANDROID_HOME/platform-tools/adb
$ADB shell am start -n com.raemond.opentrailpaper/.ui.MainActivity   # first run → tutorial
$ADB exec-out screencap -p > raw-00-first.png                          # Welcome
$ADB shell input tap 540 2190; $ADB exec-out screencap -p > raw-tut-1.png   # What you'll do
# finish the tutorial, then tabs sit at y=2240: Ride 100, Route 320, Rides 540, Mesh 760, Settings 980
$ADB shell am start -n com.raemond.opentrailpaper/.ui.MainActivity --ez demo-dash true  # populated dashboard
```

Then pad: paste each 1080 × 2400 capture at x=135 on a 1350 × 2400 `#F2F0E8` canvas.
