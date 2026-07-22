# App Store screenshots

Ready-to-upload screenshots for App Store Connect, captured at **1320 × 2868**
— the 6.9" iPhone display size class (iPhone 16 Pro Max), which Apple accepts
for the required largest-iPhone slot.

| File | Screen |
|------|--------|
| `01-plan-a-route.png` | Route tab with a searched destination previewed |
| `02-review-rides.png` | Rides list |
| `03-tune-and-update.png` | Settings with a firmware update pending |
| `04-tutorial-welcome.png` | First-run tutorial — welcome |
| `05-tutorial-location.png` | First-run tutorial — location permission |
| `06-tutorial-bluetooth.png` | First-run tutorial — Bluetooth permission |

## Regenerating

The app reads launch arguments to pose each screen deterministically (no live
device or Bluetooth needed). Boot the 6.9" simulator, set a clean status bar,
then launch with the matching flags:

```sh
SIM=$(xcrun simctl list devices available | grep -m1 "iPhone 16 Pro Max" | grep -oE "[0-9A-F-]{36}")
BID=com.raemond.opentrailpaper

xcrun simctl boot "$SIM"; xcrun simctl bootstatus "$SIM" -b
xcrun simctl status_bar "$SIM" override --time "9:41" \
  --batteryState charged --batteryLevel 100 \
  --cellularMode active --cellularBars 4 --wifiBars 3

# build + install first:  see ../README.md (xcodegen generate; xcodebuild …)

shoot() { xcrun simctl terminate "$SIM" "$BID" 2>/dev/null; \
          xcrun simctl launch "$SIM" "$BID" "${@:2}" >/dev/null; sleep 5; \
          xcrun simctl io "$SIM" screenshot "$1"; }

shoot 01-plan-a-route.png      -tab-route -demo-route
shoot 02-review-rides.png      -tab-rides -demo-rides
shoot 03-tune-and-update.png   -tab-settings -demo-update
shoot 04-tutorial-welcome.png  -onboarding-step 0
shoot 05-tutorial-location.png -onboarding-step 2
shoot 06-tutorial-bluetooth.png -onboarding-step 3
```

Launch flags: `-tab-{route,rides,settings}` pick the tab; `-demo-route`,
`-demo-rides`, `-demo-update` seed fake data so the screens are populated
without a paired device; `-onboarding[-step N]` opens the first-run tutorial.
