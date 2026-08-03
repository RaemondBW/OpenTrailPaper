# CPU Sleep — Option A: rebuild the Arduino **core 2.x** libs with PM enabled

**Goal:** turn the already-in-tree light-sleep wiring (`src/power_mgmt.{h,cpp}`,
called from `main.cpp`) from a graceful no-op into a live light-sleep config —
**without** migrating to Arduino core 3.x / pioarduino. We stay on
`espressif32@6.5.0` (Arduino 2.0.14) / can bump to `@6.13.0` (2.0.17), and rebuild
just the precompiled ESP32-S3 libraries with `CONFIG_PM_ENABLE=y` (+ tickless
idle + BT modem sleep) using **`espressif/esp32-arduino-lib-builder`**.

**Status of THIS run:** I produced the complete, verified-as-far-as-safe build
recipe (exact repo/branch, the precise IDF-4.4 sdkconfig deltas, the PlatformIO
integration diff) and settled the BLE-clock question from the vendor config. I
**did not execute the multi-GB from-source build** in this environment because
**Docker's daemon is down and the disk is at 96% (18 GB free)** — starting a
full IDF-4.4 install + S3 build there risks filling the user's disk. The exact
runnable script to finish it on a machine with headroom is in §2. Nothing was
flashed.

---

## 0. Why the stock framework can't sleep (confirmed this run)

`~/.platformio/packages/framework-arduinoespressif32` is
`3.20017.241212` = **Arduino core 2.0.17, ESP-IDF 4.4.7** (verified:
`tools/sdk/esp32s3/include/esp_common/include/esp_idf_version.h` → 4.4.7).

In `tools/sdk/esp32s3/sdkconfig`:
| Option | Stock | Line |
|---|---|---|
| `CONFIG_PM_ENABLE` | `# ... is not set` | 1147 |
| `CONFIG_FREERTOS_USE_TICKLESS_IDLE` | absent | — |
| `CONFIG_BT_CTRL_MODEM_SLEEP` | `# ... is not set` | 467 |
| `CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240` | `=y` | 910 |
| `CONFIG_ESP32S3_RTC_CLK_SRC_INT_RC` | `=y` | 988 |

`nm tools/sdk/esp32s3/lib/libesp_pm.a` shows `esp_pm_configure` present as a
**defined stub** → the drafted firmware links & runs on stock, it just gets
`ESP_ERR_NOT_SUPPORTED` and logs "PM unavailable". Confirmed the drafted
`src/power_mgmt.cpp` handles exactly that (`begin()` returns false on
`ESP_ERR_NOT_SUPPORTED`).

### The precompiled-lib layout that matters for integration
`tools/sdk/esp32s3/` has a common `lib/` (holds **`libesp_pm.a`**, 171 KB) plus
per-memory-type override dirs. The board is `qio_opi`
(`vendor/.../boards/T5-ePaper-S3.json` → `"memory_type": "qio_opi"`), so the app
links `lib/*` **overridden by** `qio_opi/*`, which contains only the
PSRAM-mode-sensitive libs:
```
qio_opi/  libbootloader_support.a  libesp_hw_support.a  libesp_system.a
          libfreertos.a  libspi_flash.a  sections.ld  include/sdkconfig.h
```
Verified `qio_opi/include/sdkconfig.h` has `CONFIG_SPIRAM_MODE_OCT 1` +
`CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240 1` but **no** `CONFIG_PM_ENABLE` /
`FREERTOS_USE_TICKLESS_IDLE` / `BT_CTRL_MODEM_SLEEP`. So PM is off in both the
compiled `.a`s and the compile-time header. A correct rebuild must regenerate
**both** the common `lib/` and the `qio_opi/` override (esp. `libfreertos.a`,
which carries tickless idle) — the lib-builder does this automatically (see §1).

### IMPORTANT naming correction vs the prior spike
The prior spike's `sdkconfig.defaults.pm` uses **IDF-5.x** Kconfig names
(`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240`, `CONFIG_RTC_CLK_SRC_INT_RC`,
`CONFIG_SPIRAM_MODE_OCT`, `CONFIG_SPIRAM_LEAKAGE_WORKAROUND`) because it targeted
the pioarduino/IDF-5 path. **For Option A (IDF 4.4.7) the names are different** —
S3-scoped options are `ESP32S3`-prefixed. Use the deltas in §1.2 below, not the
IDF-5 file.

---

## 1. The lib-builder build

### 1.1 Repo & branch
- Repo: `https://github.com/espressif/esp32-arduino-lib-builder`
- Branch: **`release/v4.4`** (there is no `idf-release_v4.4` *tag*; the 4.4 line
  lives on the branch, HEAD `3eb9cb06` as of this run). This branch's
  `tools/install-esp-idf.sh` checks out **ESP-IDF v4.4.x** and Arduino core
  **2.0.x**, matching the framework we ship against. (The `idf-release_v5.x`
  tags are the 3.x line — do NOT use them for Option A.)

### 1.2 The sdkconfig deltas (IDF 4.4.7 names)
Realized as a config fragment `configs/defconfig.pm` (build.sh appends any config
name you pass as a positional arg → `configs/defconfig.<name>`). Exact contents:

```ini
# ---- Power management: automatic light sleep, NO DFS ----
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3
CONFIG_PM_SLP_IRAM_OPT=y
CONFIG_PM_RTOS_IDLE_OPT=y
# Keep CPU pinned at 240; runtime uses esp_pm_configure(min==max==240) so DFS
# never clocks the APB down while 80 MHz OPI PSRAM is live (avoids the boot loop):
CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240=y
# RTC slow clock = internal 150 kHz RC (board has NO 32k xtal on the S3 — §4):
CONFIG_ESP32S3_RTC_CLK_SRC_INT_RC=y
# BLE controller modem sleep, LP clock from RTC slow (survives light sleep):
CONFIG_BT_CTRL_MODEM_SLEEP=y
CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1=y
CONFIG_BT_CTRL_LPCLK_SEL_RTC_SLOW=y
# Sleep leakage workarounds (already on in stock; keep explicit):
CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND=y
CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND=y
```

Notes on the choices:
- `CONFIG_PM_DFS_INIT_AUTO` does **not** exist in IDF 4.4 (it's IDF-5). DFS is
  suppressed purely at runtime by `min_freq_mhz == max_freq_mhz == 240` in
  `power_mgmt::begin()` — no build option needed. Do **not** add it.
- Leave `CONFIG_SPIRAM_MODE_OCT` / `CONFIG_SPIRAM_SPEED_80M` to the lib-builder's
  own `configs/defconfig.opi_ram` + `defconfig.80m` (the `opi_ram` mem-variant),
  which is exactly how the stock `qio_opi` libs are produced. Don't pin PSRAM
  mode in `defconfig.pm` — it must stay per-mem-variant.
- Put `defconfig.pm` in the build so it lands in **every S3 sub-build**. build.sh
  builds each variant as `defconfig.common;defconfig.esp32s3;<mem/flash>` — the
  cleanest way to force PM into all of them (incl. the `qio_opi` `libfreertos.a`
  and the generated `qio_opi/include/sdkconfig.h`) is to append PM to the S3
  base. Either (a) `cat configs/defconfig.pm >> configs/defconfig.esp32s3`, or
  (b) pass `pm` as a trailing config to a `-b` sub-build. Option (a) is the
  reproducible one for a full `-t esp32s3` run (see script).

### 1.3 The runnable build script (finish on a box with disk + Docker/toolchain)
Save as `build-pm-libs.sh`, run **outside the git repo** (e.g. `~/lib-builder-pm`):

```bash
#!/usr/bin/env bash
set -euo pipefail
WORK="$HOME/lib-builder-pm"            # NOT inside the tdisplay worktree
rm -rf "$WORK" && mkdir -p "$WORK" && cd "$WORK"

git clone -b release/v4.4 --recurse-submodules \
    https://github.com/espressif/esp32-arduino-lib-builder .

# --- inject PM config into the ESP32-S3 base defconfig ---
cat >> configs/defconfig.esp32s3 <<'EOF'
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3
CONFIG_PM_SLP_IRAM_OPT=y
CONFIG_PM_RTOS_IDLE_OPT=y
CONFIG_ESP32S3_RTC_CLK_SRC_INT_RC=y
CONFIG_BT_CTRL_MODEM_SLEEP=y
CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1=y
CONFIG_BT_CTRL_LPCLK_SEL_RTC_SLOW=y
CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND=y
CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND=y
EOF
# (CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240=y is already present in that file.)

# Build ONLY the esp32s3 target (idf_libs + bootloaders + all 5 mem variants).
# First run installs ESP-IDF 4.4.x + the xtensa toolchain (~3–4 GB) and compiles
# everything (~15–40 min). Needs Python 3, git, cmake, ninja, jq (all present
# here except idf.py, which install-esp-idf.sh provides).
./build.sh -t esp32s3

# Output tree lands under:
#   components/arduino/tools/esp32-arduino-libs/esp32s3/   (the new tools/sdk/esp32s3)
# containing lib/, qio_opi/, include/, ld/, sdkconfig, flags/, etc.
```

**Docker alternative (recommended when the host toolchain is uncooperative):**
```bash
docker run --rm -v "$PWD":/work -w /work \
    espressif/esp32-arduino-lib-builder:release-v4.4 \
    /bin/bash -c "cat configs/defconfig.pm >> configs/defconfig.esp32s3 && ./build.sh -t esp32s3"
```
(Requires the Docker daemon running — it is **down** here.)

**Verify the rebuilt libs before integrating** (on the build box):
```bash
S3=components/arduino/tools/esp32-arduino-libs/esp32s3
grep -E 'CONFIG_PM_ENABLE|FREERTOS_USE_TICKLESS_IDLE|BT_CTRL_MODEM_SLEEP' $S3/sdkconfig
grep -E 'CONFIG_PM_ENABLE|CONFIG_SPIRAM_MODE_OCT' $S3/qio_opi/include/sdkconfig.h
# Expect CONFIG_PM_ENABLE=y in both, plus SPIRAM_MODE_OCT in the qio_opi header.
xtensa-esp32s3-elf-nm $S3/lib/libesp_pm.a | grep esp_pm_impl_switch_freq  # now the real impl, not the stub
```

---

## 2. PlatformIO integration (in the worktree)

Copy the rebuilt S3 SDK tree over a **local** copy of the framework package (so
the stock global package is untouched), then point PlatformIO at that copy.

```bash
# from the repo root
FWSRC=~/.platformio/packages/framework-arduinoespressif32
cp -R "$FWSRC" ./framework-arduinoespressif32-pm
# overlay the rebuilt S3 sdk (from the build box output):
rsync -a --delete \
  ~/lib-builder-pm/components/arduino/tools/esp32-arduino-libs/esp32s3/ \
  ./framework-arduinoespressif32-pm/tools/sdk/esp32s3/
```

`platformio.ini` change (exact diff):
```diff
 [env:t5s3-pro]
 platform = espressif32@6.5.0
 board = T5-ePaper-S3
 framework = arduino
+; Use the locally-rebuilt PM-enabled Arduino 2.0.17 libraries. Keeps the
+; espressif32@6.5.0 platform + build scripts; only the precompiled tools/sdk
+; is swapped. Path is relative to this ini file.
+platform_packages =
+    framework-arduinoespressif32 @ file://./framework-arduinoespressif32-pm
 upload_speed = 921600
 monitor_speed = 115200
 monitor_filters = esp32_exception_decoder
```

Also drop the dead PSRAM define while here (confirmed consumed nowhere; prior
spike §0):
```diff
 build_flags =
     -DBOARD_HAS_PSRAM
-    -DPSRAM_SPEED=120MHz
+    ; -DPSRAM_SPEED=120MHz  ; dead define; OPI PSRAM is 80 MHz
```

`.gitignore` the local copy: `framework-arduinoespressif32-pm/` (it's ~200 MB;
the reproducible input is the script in §1.3, not the binary tree). If you'd
rather vendor it, commit only `tools/sdk/esp32s3/{lib,qio_opi,include,ld,sdkconfig}`.

> The `power_mgmt.{h,cpp}` + `main.cpp` hooks + the drafted `sdkconfig.defaults.pm`
> live in the **main working tree** (uncommitted), not on committed `main` and
> not in this worktree (which branches from an earlier commit). Before building,
> make sure `src/power_mgmt.{h,cpp}` and the `power_mgmt::begin()/tick()` calls
> are present in whatever tree you compile.

---

## 3. Verification

**Done this run:**
- Framework identified as Arduino 2.0.17 / IDF 4.4.7; PM confirmed compiled out
  (sdkconfig + `qio_opi/include/sdkconfig.h` + `nm libesp_pm.a` stub).
- lib-builder `release/v4.4` cloned & inspected; build system traced end-to-end
  (`build.sh` assembles `defconfig.common;defconfig.$target;<variant>` for
  `idf_libs` **and** each `mem_variant`, so a base-level PM delta reaches the
  `qio_opi` libs). `configs/defconfig.pm` fragment authored with correct 4.4
  names. jq/cmake/ninja present.
- BLE LP-clock question resolved from hardware config (§4).

**NOT done (blocked by environment):** the actual from-source compile and a
`pio run -e t5s3-pro` against the rebuilt framework. **Reason:** Docker daemon
down + disk at 96% (18 GB free) — a full IDF-4.4 install + S3 build could exhaust
the disk and harm the user's system. No safe way to produce real PM-enabled `.a`s
here. (A header-only flip of `CONFIG_PM_ENABLE` without rebuilding `libesp_pm.a`
was deliberately NOT done — it would compile but still call the disabled stub, a
misleading half-state.)

**Post-build verification checklist (once §1.3 runs on a capable box):**
1. `grep CONFIG_PM_ENABLE $S3/qio_opi/include/sdkconfig.h` → `=y`.
2. `pio run -e t5s3-pro` against the §2 framework → still links (the drafted
   firmware already compiles on stock; PM symbols are unchanged in signature).
3. Boot log check (no flash — read serial only if someone is at the device):
   `power_mgmt::begin()` should now log
   `pm: esp_pm_configure(min=max=240, light_sleep=1) -> ESP_OK`
   instead of `pm: light sleep UNAVAILABLE`.

---

## 4. BLE low-power-clock decision — RESOLVED

**Decision: internal 150 kHz RC, `CONFIG_ESP32S3_RTC_CLK_SRC_INT_RC=y`, and BLE
controller LP clock `CONFIG_BT_CTRL_LPCLK_SEL_RTC_SLOW=y`. Do NOT select
EXT_CRYS / EXT_32K_XTAL.**

Evidence the board has **no 32.768 kHz crystal on the ESP32-S3**:
- Vendor factory/grayscale sdkconfig
  (`vendor/.../examples/grayscale_test/sdkconfig`) ships
  `CONFIG_ESP32S3_RTC_CLK_SRC_INT_RC=y` with `EXT_CRYS`/`EXT_OSC` **not set** —
  LilyGO's own config uses the internal RC.
- The pinmap (`vendor/.../docs/pinmap.md` §5) lists only the **PCF8563TS** RTC on
  I²C (SDA=39, SCL=40, INT=2, VRTC via the J12 coin cell). No ESP32-S3
  `32K_XP`/`32K_XN` crystal is wired. The PCF8563 is a *separate I²C* RTC and
  does not feed the SoC's RTC slow clock.

Why RTC_SLOW (not main XTAL): the main XTAL is **gated during light sleep**, so
`CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL` would force the BLE controller to hold a
no-light-sleep lock → **no CPU sleep while connected**. Sourcing the controller's
LP clock from the RTC slow clock (fed by the internal RC) keeps it ticking through
sleep. **Caveat:** the internal RC drifts ~5%, widening BLE wake windows slightly
— a little extra radio power and, at long connection intervals, a small risk of
supervision-timeout disconnects. Mitigations: keep a generous supervision timeout
(the drafted `updateConnParams(... 400)` = 4 s) and measure (§5C). Fitting a 32k
crystal to the S3 is the only way to tighten this, and is out of scope.

---

## 5. On-bench measurement plan (needs an inline current meter; do after flashing)

Use a Nordic PPK2 or µCurrent+scope on the battery lead. Average current over
≥60 s per condition; scope the sleep/active duty cycle if possible.

**A. Idle draw, before vs after (the headline).**
1. Stock framework, on the desk, GPS on, BLE advertising, no phone, screen static
   → record mA.
2. PM framework, same → expect the CPU/idle portion to drop as the core sleeps
   between the ~30 ms UI polls. Report the absolute mA delta (radio/PSRAM
   self-refresh/EPD bias/sensors are unchanged, so the delta is the CPU win).
3. Phone connected: (a) legacy 15–30 ms conn interval, (b) relaxed 150–300 ms
   idle interval (if the `ble_server.cpp` relax change is in). Quantify the
   connection-interval contribution.

**B. GPS reliability (must not regress).** Cold + warm start, PM on vs off: TTFF
and fix hold. Watch the SD diag `gps ...: chars= ck=ok/bad`; a rising bad-checksum
count or stalled `chars` under PM = FIFO overflow (dropped NMEA). The GPS task's
50 ms poll bounds every sleep to ~48 bytes (< 128-byte HW FIFO) at 9600 baud, so
no loss is expected. If it regresses, enable the UART1 wake path
(`#define PM_GPS_UART_WAKEUP` — requires moving `SerialGPS` to `Serial1`, since
the S3 can only wake from UART0/1). Ride ≥15 min moving vs a PM-off control.

**C. BLE stability.** Phone connected 30+ min at the idle interval; count
unexpected disconnects (diag "phone disconnected reason"), esp. supervision
timeout (0x08) — this is where the RC drift would show. Run a full ride
download / OTA / map upload with PM on: verify the fast interval snaps back
(throughput unchanged) and re-relaxes after.

**D. Map-render time (guard vs a PSRAM regression).** PSRAM stays 80 MHz OPI, so
render time should be unchanged. Time a fixed map pan/redraw PM-off vs PM-on;
confirm ~0 delta. (This is the go/no-go metric if DFS/40 MHz PSRAM is ever
explored — not now.)

**E. Wake correctness.** Touch + both buttons wake and respond after a long idle;
the 10 min deep-sleep auto-off still fires; USB replug mid-idle restores the
console (the no-sleep lock re-acquires).

---

## 6. Remaining steps to finish (ordered)
1. On a machine with Docker **or** ~15 GB free disk, run the §1.3 script
   (`./build.sh -t esp32s3`) → produces the PM-enabled `esp32s3` SDK tree.
2. Run the §1.2 verify greps on the output (`CONFIG_PM_ENABLE=y` in `sdkconfig`
   **and** `qio_opi/include/sdkconfig.h`).
3. Overlay into a local framework copy + apply the §2 `platformio.ini` diff.
4. Ensure `src/power_mgmt.{h,cpp}` + the `main.cpp` hooks are in the tree, then
   `pio run -e t5s3-pro` → expect a clean link.
5. (Someone at the device) flash, read serial for the `-> ESP_OK` PM log, then
   run the §5 bench plan.
