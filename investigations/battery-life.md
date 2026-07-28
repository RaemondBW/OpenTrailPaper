# Battery life — measurements, attempts, and where we stalled

Status as of 2026-07-27. **No power optimisation has been measured on hardware
yet.** Everything below is either a measurement of the *current* drain, or an
attempt whose effect was never quantified. Read the "Honest status" section
before trusting any number in the middle of this document.

## 1. The measured baseline

From a real ride-day diagnostic log (`bikegps-diag 6.log`, 108 battery samples
over 3.73 h on a 1500 mAh cell, firmware v0.84, backlight OFF for the logged
period):

| Metric | Value |
|---|---|
| Mean discharge current | **−181 mA** (median −182, σ 11.6) |
| Active riding | −185 to −190 mA |
| Idle (GPS searching, not recording) | **−162 to −166 mA** |
| SOC slope | 100 % → 67 %, i.e. −12.2 %/h |
| Runtime from full | **~8.2 h** to empty, ~7.4 h usable |

**The structural finding that shapes everything else: the baseline is ~165 mA
and active riding only adds ~20 mA.** The device is expensive *just sitting
there powered on*. The levers that matter are the always-on consumers, not the
per-second refresh.

### Apportioning the ~165 mA

No per-rail instrumentation exists, so this is datasheet-plus-measured-delta.
The firm anchors are the idle baseline, the small idle→active delta, and the
board schematic.

| Subsystem | Est. mA | Confidence | Notes |
|---|---|---|---|
| **CPU @ 240 MHz + BT, never light-sleeps** | 55–80 | medium | Every task busy-polls; FreeRTOS idle almost never runs |
| **Backlight** | 25–55 | medium/high | Only if on; default was level 2 of 3 |
| **GPS (CASIC + 3V3 rail)** | 20–30 | high | Rail driven HIGH at boot, held on continuously |
| E-paper refresh (1 Hz DU) | 10–20 | medium | Panel powered off between passive updates |
| BLE link + sensor scan | 8–18 | medium | |
| LoRa (SX126x, unused) | 1–5 | low | Only CS-deselected, never slept |
| Fuel gauge / RTC / IO expander / touch | 2–5 | low | |

Two dominant, *avoidable* chunks: **backlight** (policy) and **CPU never
sleeping** (architecture).

### Log anomalies worth knowing

- **The fuel gauge's `charging`/`discharging` flag is unreliable.** Samples read
  "charging" while current is clearly negative. **Trust the sign of the current,
  not the word.**
- **Frequent `power-on / power-loss` resets.** 34 across 25–26 July vs only 2
  real panics. Most are flash cycles and manual resets, but they are
  indistinguishable from brownouts in the log, so they are not proof of
  stability either way.

## 2. What was attempted

### Landed and shipping

- **Stop scanning for unpaired sensors mid-ride.** `allConnected` counted all
  three sensor kinds, so with only an HR strap paired the radio active-scanned
  for the entire ride hunting for a power meter that did not exist. Now only
  *paired* kinds gate scanning. Est. 5–12 mA. **Not measured.**
- **Backlight default.** Now defaults off rather than level 2.

### Written but never fairly tested

Both of these live on `power-quick-wins`, which was **compiled by a corrupted
local toolchain** (see §4). Their effect is unknown — they need rebuilding and
re-testing on a clean environment before any claim is made about them.

- **BLE idle connection interval** (`ble-idle-conn-params`, also merged into
  `power-quick-wins`). The link requested 15–30 ms with zero slave latency for
  every connection, to keep bulk transfers fast — but the steady state is a 1 Hz
  status notify, so that woke both device and phone ~33×/s for nothing. Now
  starts fast, relaxes to 150–300 ms once transfers go quiet, snaps back on
  activity (3 s trailing window). Est. 3–10 mA.
- **UI task wake-on-input.** The UI task ran a fixed 30 ms tick (~33 wakeups/s,
  the highest-frequency waker) even though touch and buttons are *already*
  interrupt-driven with a 200 ms fallback poll. The tick bought no
  responsiveness; it kept the FreeRTOS idle task — which halts the core with
  `waiti` — from ever running. Now the ISRs wake the task and it blocks up to
  250 ms otherwise.

  **Implementation note that cost real debugging time:** the first version
  signalled the task through its *built-in notification slot*
  (`vTaskNotifyGiveFromISR` / `ulTaskNotifyTake`). ESP-IDF drivers use the
  **calling task's** notification slot to wait for completion — including the
  SPI master behind the SD card and epdiy's DMA waits — and the UI task does
  both SD work and panel drawing. A touch interrupt landing mid-transfer
  corrupts a driver's completion handshake. **Use a dedicated semaphore, never
  the task notification slot, for anything that shares a task with a driver.**

### Not attempted

- **GPS duty-cycling when parked.** Up to 20–30 mA while stopped, ~0 while
  riding. Caveat: the rail was deliberately tuned for first-fix time, and
  cutting power loses the hot-start RAM. Only worth engaging when clearly idle.
- **Sleeping the unused LoRa SX126x at boot.** 1–5 mA. Skipped because only
  `BOARD_LORA_CS` is defined (no RST/BUSY pins), so it needs hand-rolled SPI
  commands on the bus the SD card shares. Poor risk/reward.

## 3. CPU light sleep — the big lever, still unclaimed

Worth 55–80 mA, and the reason for most of the effort. Two routes were pushed
a long way; **neither works.**

### Route A — rebuild the Arduino framework with `CONFIG_PM_ENABLE`

The stock PlatformIO Arduino framework compiles power management *out*, so
`esp_pm_configure()` returns `ESP_ERR_NOT_SUPPORTED` and the CPU never sleeps.
The fix is to rebuild the precompiled libraries with PM enabled.

This **succeeded as a build** after six CI iterations
(`.github/workflows/build-pm-libs.yml`), and the sequence of failures is worth
recording because it is all one root cause:

> `esp32-arduino-lib-builder`'s `release/v4.4` branch clones its component
> dependencies at their *branch HEADs*, which have all drifted past IDF 4.4.

| Failure | Cause | Fix |
|---|---|---|
| `idf >=5.0` dep-solve conflict | idf-component-manager 1.5.x injects it | `IDF_COMPONENT_MANAGER: "0"` |
| `Failed to resolve component 'esp_mm'` | camera/dsp HEADs need an IDF-5 component | **prune** camera, esp-dl, esp-dsp, rainmaker — we link none of them |
| `Failed to resolve component 'esp_partition'` | esp_littlefs HEAD | pin littlefs |
| `esp_secure_cert_read.h` missing | esp-rainmaker | pruned |
| tinyusb source layout | HEAD moved to dwc2 | pin tinyusb |
| `esp_vfs_littlefs_conf_t has no member 'partition'` | pinned littlefs *too far back* | v1.10.2 — has the field, and on IDF<5 requires `spi_flash` not `esp_partition` |
| Wrong artifact path | IDF-4.4 writes `out/tools/sdk/esp32s3` | fix the `find` |

**Lesson: prune the components you do not link rather than pinning them one by
one.** Four of the seven failures vanished at once when we stopped chasing pins.

Integration then needed two more fixes: strip the pruned libs from the
framework's hardcoded link list in `tools/platformio-build-esp32s3.py`, and
remove `-Wl,--wrap=esp_log_write/esp_log_writev/log_printf` — those wrappers are
defined in `libesp_diagnostics.a`, which came from a pruned component.

**Then it linked, flashed, ran — and corrupted the e-paper image.** The
framebuffers live in OPI PSRAM, and light sleep disturbs them. Two of the only
two real panics in the logs came from this build (`esp_pm_configure(...) ->
ESP_OK` immediately before, one crashing in the `ui` task). Rolled back.

Branch `cpu-light-sleep`. The libs build reproducibly via the workflow.

### Route B — port to ESP-IDF

Owning `sdkconfig` directly is the durable fix. Staged approach: keep the
Arduino core as an IDF component so ~9.5k lines compile unchanged, then shed
Arduino subsystem by subsystem.

**It builds a complete firmware binary with `CONFIG_PM_ENABLE=y`, tickless idle,
BT modem sleep on the internal RC clock, OPI PSRAM preserved — and it does not
boot.** No serial output, undiagnosed.

Toolchain notes (all load-bearing, see `PORTING.md`):

- **ESP-IDF must be ≥ v5.4.2, not v5.4.1.** arduino-esp32 3.2.x guards an
  I2C-slave LL rename with `ESP_IDF_VERSION >= 5.4.2`, but the rename actually
  landed in 5.4.1 — so on *exactly* 5.4.1 the Arduino core will not compile.
- **epdiy must come from upstream git**, not the LilyGO-vendored copy (2.0.0
  predates IDF 5.3's LCD peripheral rename). Safe: we drive `epd_board_v7` +
  `ED047TC1`, both upstream.
- **NimBLE via `h2zero/esp-nimble-cpp`** with `CONFIG_ARDUINO_SELECTIVE_BLE=n` —
  our code uses the `NimBLE*` API and Arduino 3.x's bundled `BLE*` library
  collides.
- USB-MSC and `usb_persist_restart` are stubbed (`OTP_NO_USB_MSC`) because
  `arduino_tinyusb` is not published to the IDF component registry.

**The next step is not more code — it is a boot log.** The console is configured
to USB-CDC, which needs the TinyUSB stack that build stubs out, so it almost
certainly has nowhere to write. Route it to USB-Serial-JTAG and the panic will
be visible on port `1101`.

Branch `esp-idf-port`.

### Not viable

**160 MHz downclock** — documented in `main.cpp`: the OPI PSRAM panicked at the
lower clock and the device boot-looped. Also why the PM config deliberately
pins `min == max == 240 MHz` with no DFS: we want power-gating between the
~1 Hz workloads, not a slower clock.

## 4. The build-environment incident (why the numbers above are untrusted)

While iterating on Route A, the local PlatformIO framework package was deleted
and reinstalled repeatedly, and a PM-modified copy was installed over the
unversioned `framework-arduinoespressif32` slot. **The local toolchain ended up
producing binaries that did not work** — most visibly, the SD card would not
mount on *any* locally built firmware, including two-week-old commits.

This was diagnosed only after a CI-built binary of the identical commit worked
perfectly. Crucially, **every package version string matched between local and
CI** — platform 6.5.0, framework 3.20014.231204, toolchain 8.4.0, NimBLE 2.5.0 —
so a version audit said "clean" while the bytes on disk were not.

Repaired by deleting `~/.platformio/packages/framework-arduinoespressif32*` and
the whole `.pio` directory (build cache *and* `libdeps`), then rebuilding.

**Two lasting lessons:**

1. **Matching version strings do not mean matching content.** If a package has
   been hand-modified, uninstall and re-download it; do not audit it in place.
2. **CI is the reference build.** `.github/workflows/build.yml` produces a
   firmware artifact from a clean environment. When on-device behaviour is
   inexplicable, flash the CI artifact before theorising — it costs one flash
   and eliminates an entire category of doubt.

Several hours of "SD card" debugging in this session were chasing this phantom.

## 5. Honest status

- **Nothing has been measured.** The 3–10 mA and 55–80 mA figures are estimates.
  No before/after comparison against the ~165 mA idle / ~181 mA active baseline
  has been run.
- The device already logs signed battery mA every ~2 minutes. **That is the
  measurement rig** — a quiet 20 minutes, or a ride, is enough.
- The two branches with real savings potential (`power-quick-wins`) were built
  by the corrupted toolchain and must be rebuilt and re-tested before any claim.
- CPU light sleep, the single largest lever, remains unavailable by both routes.

**Suggested order if picked up again:** rebuild `power-quick-wins` cleanly →
measure it → then decide whether the IDF port is worth finishing, based on
whether the app-level savings already got you where you need to be.
