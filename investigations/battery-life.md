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

### Landed 2026-07-31 (from `bikegps-diag 7.log`, v0.85/0.86, 163 samples)

That log reconfirms the shape — grouping every `battery:` sample by what was
actually running at the time:

| GPS fix | recording | phone | HR | power | n | mean |
|---|---|---|---|---|---|---|
| ✓ | ✓ | ✓ | ✓ | ✓ | 48 | 194 mA |
| ✗ | ✗ | ✓ | ✗ | ✗ | 21 | 177 mA |
| ✗ | ✗ | ✓ | ✓ | ✓ | 43 | 170 mA |
| ✗ | ✗ | ✗ | ✗ | ✗ | 7 | **167 mA** |

**BLE is not the problem and should not be optimised further**: three
simultaneous links (phone + HR + Assioma) cost 3 mA over nothing connected.
Doing the actual job — fix, map redraw, FIT logging — costs ~27 mA. Session mean
183 mA; 848 mAh burned in 4 h 37 m; ~8 h to empty. Same story as `diag 6`.

Two new things the log shows:

- **Deep sleep is ~2.5 mA, not µA.** One long window (18:28:46 → 19:39:34,
  70.8 min) burned 7 mAh; back out the 73 s of runtime before sleep and the
  wake/boot and it is ~3 mAh over 1.2 h. The `mAh` column is a genuine coulomb
  count (it tracks the reported mA to within 1 %), so this is real. Something —
  GPS backup rail, SD, EPD boost, the XL9555 — stays powered in "off". Standby
  is weeks, not months. Not chased yet.
- **The GPS UART runs at 761 chars/s / 16.0 NMEA sentences/s, continuously**,
  measured over 18,876 s. Every sentence type for every constellation, 24/7,
  including 5 h indoors at `sats=0`. Trimming to GGA+RMC+GSA is free and cuts
  the RX interrupt rate ~4x; that matters once the CPU can actually sleep.

**AID-INI spam — fixed.** 48 aiding injections in one session, gaps as short as
5 s, mostly the *identical* coordinate (`37.7698,-122.4377` fourteen times in a
row). There was already a 20 s rate limit, but (a) the boot seed didn't prime
it, so the first phone seed always slipped through ~5 s later, and (b) 20 s is
meaningless for a one-shot acquisition aid — each AID-INI restarts the
receiver's search, so re-seeding a searching receiver actively delays the fix it
is meant to speed up. TTFF in that log was **716 s on an aided warm start**, and
the other indoor sessions never fixed at all. `aidingIsNews()` in
`gps_service.cpp` now accepts a phone seed only when it carries information the
receiver doesn't already have (first seed / gained time / ≥2x tighter accuracy /
moved outside the uncertainty circle we already gave it, that last one floored
at 60 s). Withheld seeds are counted and reported on the `gps FIRST FIX` line,
so the gate is visible in the log rather than indistinguishable from the phone
having gone quiet. **Effect on TTFF not yet measured** — that is the next ride.

### Landed 2026-08-02 — PM is ON, and the UI task no longer caps sleep

**CPU light sleep finally runs.** The `build-pm-libs.yml` artifact was installed
whole (it contains the entire `tools/sdk/esp32s3/` tree, so the partial-overlay
hazard in `pm-rebuild-baseline.md` §3 never applied), and the device boots with
`esp_pm_configure(min=max=240, light_sleep=1) -> ESP_OK`. Verified by A/B with
the new `-DPM_LIGHT_SLEEP=0` switch: PM linked and configured but not sleeping,
then sleeping. SD mounted in both. Route A of §3 is no longer blocked.

Two of our own bugs had to be fixed first, and both would bite any future PM
build:

- **`power_mgmt::tick()` deadlocked the console.** It held the no-light-sleep
  lock only while `(bool)Serial` was true — but that is `USBCDC::connected`,
  which `USBCDC.cpp:236` sets only when a host asserts **both DTR and RTS**, and
  light sleep gates the USB PHY before any host can attach. The condition the
  guard depended on was destroyed by the thing it guarded against. Now: lock
  acquired inside `begin()` (closing the setup→loop gap) plus a 30 s boot grace
  window. The durable fix is to key off VBUS via the BQ25896 rather than
  `Serial`; not done.
- **The SD path takes no PM lock.** `sd_diskio.cpp` drives the card through
  Arduino's `SPIClass` → `esp32-hal-spi.c`, which has **zero** references to
  `esp_pm_lock`, where IDF's own `spi_master` in `libdriver.a` has 32. So light
  sleep could land mid-transaction. Fixed by holding `ESP_PM_NO_LIGHT_SLEEP`
  across `sdLock()`/`sdUnlock()` (`power_mgmt::busyAcquire/busyRelease`), which
  covers every SD access in the firmware since they all go through that guard.
  Known gap: `SPI.begin()` in `ride_recorder::begin()` is still outside it.

**UI task is now wake-on-input**, which is what makes light sleep pay. It ran a
flat 30 ms tick — ~33 wakeups/s — while everything inside the loop was already
interrupt-driven or on a slower timer (redraw 1 Hz, touch-poll fallback 200 ms,
elevation 2.5 s). Since the SoC only sleeps when **both cores** are idle and
tickless idle sizes each sleep by the shortest pending timer across them, that
tick capped every sleep on core 1 at 30 ms. Now it blocks on a dedicated
semaphore with a 200 ms fallback. Per the warning above in §2, it is a
semaphore, **not** the task notification slot.

**The next constraint is the GPS task's 50 ms poll on core 0**, which now sets
the sleep ceiling. It is deliberate — it bounds each sleep so the 128-byte UART
FIFO cannot overflow at ~960 byte/s. Raising it needs `esp_sleep_enable_uart_wakeup()`,
which only works on UART0/1 while `SerialGPS` is UART2. **Watch `ck=good/bad` in
the GPS log after enabling sleep**: the UART is unclocked while asleep, so a
50 ms sleep drops ~48 bytes mid-sentence and bad checksums are the tell.

**Measured 2026-08-02, on battery: mean -130 mA, median -132 mA** over 45
samples (range -104 to -174), against the **167 mA idle / 183 mA session mean**
baseline. Roughly **30-50 mA saved, ~20-28%** — and understated, because the BLE
link was thrashing throughout that log (see below). GPS survives sleep fine:
`ck=1042/2`, two bad checksums per thousand sentences, so the NMEA-loss risk did
not materialise at a 50 ms sleep ceiling.

**Light sleep breaks the BLE link to the phone.** 1231 phone disconnects in one
log, ALL of them with light sleep on and none with it off, every one HCI error
0x08 (`reason 520` = NimBLE HCI base 0x200 + 0x08) — **connection supervision
timeout**. Map transfers over BLE were practically impossible.

The cause is in `sdkconfig.defaults.pm`, which warned about it: with
`CONFIG_BT_CTRL_MODEM_SLEEP` the controller sleeps between connection events and
wakes on the RTC slow clock, and `CONFIG_RTC_CLK_SRC_INT_RC` is the internal
150 kHz RC that drifts ~5%. At a 30 ms connection interval that is ~1.5 ms of
error per event — enough to miss the master's anchor point and time the link out.
The file called this "slightly widens BLE wake windows"; it is fatal.

Mitigated in firmware by holding the no-light-sleep lock while the phone is
connected (`power_mgmt::tick`). Costs little — the phone is only connected while
the app is in use, and the long tail of a ride still sleeps. The real fixes both
need a lib-builder rebuild: `CONFIG_RTC_CLK_SRC_EXT_CRYS` +
`CONFIG_BT_CTRL_LPCLK_SEL_EXT_32K_XTAL` **if** the board wires a 32.768 kHz
crystal to XTAL_32K (VERIFY against the LilyGO schematic — the PCF8563 is a
separate I2C RTC and does not count), else `CONFIG_BT_CTRL_MODEM_SLEEP=n`.

**One unexplained reset**, 2026-08-02 22:57:22, `ESP_RST_POWERON` during normal
use with the battery at 92%/4008 mV. No panic, no watchdog, no coredump — the
rail actually collapsed. A BLE disconnect storm preceded it, but that storm runs
at 40-47/min for minutes elsewhere in the same log without any reset, so it is
correlation not cause. Open: intermittent battery/JST contact, or a brownout
from a radio transient out of light sleep (the S3 has a distinct
`ESP_RST_BROWNOUT` code and we did not get it, which argues against). Single
occurrence; not reproduced.

### A day lost to a self-inflicted fault — worth reading before the next session

Most of 2026-08-01 went on an SD card that would not mount, which looked exactly
like a PM regression and was not. The chain:

1. The DTR/RTS handshake in `USBCDC::_onLineState` reboots the device into the
   **bootloader** on the sequence `!dtr&&rts → dtr&&rts → dtr&&!rts → !dtr&&!rts`.
   A serial helper that opened and closed the port in a loop walked that sequence
   and silently reset the device dozens of times, some landing mid-SPI-write.
2. That left the card's controller in a state where it refused CMD0 —
   `GO_IDLE_STATE failed`, `cardType=NONE`, and a **different garbage token each
   time** (`0x4`, `0x44`, `0x36`…), which is the giveaway that the bytes coming
   back are data, not a response.
3. It survived power cycles, reflashes and a full framework revert, so it
   persisted across every build and made PM look guilty. Reformatting the card
   cleared it (rebuilding the controller's internal tables — not a filesystem
   fix, since the failure is at CMD0, before any filesystem read).
4. It mounted fine on a Mac throughout, because a card reader power-cycles the
   card and talks native SD mode, not SPI.

Lessons: **hold DTR and RTS steady and never reopen the port** (see
`tools/` helpers); `cardType=NONE` means the card is not answering at the
protocol level, so never look at the filesystem for it; and a symptom that
survives a revert cannot be caused by the thing you reverted.

Two theories that were confidently wrong and are recorded so they are not
re-derived: "the rebuilt SDK's SPI/driver stack breaks the SD" (the sdkconfig
diff is six values and three additions, **none** touching SPI/SDMMC/driver/flash/
cache/IRAM) and "the card is stuck mid-transaction holding MISO" (the bus flush
always found it idle within 16 bytes).

### Not attempted

- **GPS duty-cycling when parked.** Up to 20–30 mA while stopped, ~0 while
  riding. Caveat: the rail was deliberately tuned for first-fix time, and
  cutting power loses the hot-start RAM. Only worth engaging when clearly idle.
- **Sleeping the unused LoRa SX126x at boot.** 1–5 mA. Skipped because only
  `BOARD_LORA_CS` is defined (no RST/BUSY pins), so it needs hand-rolled SPI
  commands on the bus the SD card shares. Poor risk/reward.

- **Not running USB when no host is attached.** *Investigated 2026-07-31 and
  deliberately not done — it is not reachable from this firmware.* `USBMSC`'s
  constructor claims the MSC interface during static init, and the Arduino core
  calls `USB.begin()` — the call that actually starts TinyUSB, the USB PHY and
  the 48 MHz USB clock domain — from `app_main()` **before `setup()` runs**,
  unconditionally: `ARDUINO_USB_ON_BOOT` is `#define`d in `USB.h` as the OR of
  the three `USB_*_ON_BOOT` flags, so it cannot be overridden from
  `build_flags`, and we need `ARDUINO_USB_CDC_ON_BOOT=1` for serial. Gating
  `usb_storage::begin()` on VBUS therefore saves exactly nothing.

  The real fix is `ARDUINO_USB_CDC_ON_BOOT=0` plus calling `USB.begin()`
  ourselves once VBUS is seen (BQ25896 `isVbusIn()` is the clean detector — the
  BQ27220 charging flag flaps and cannot be used). Cost: `Serial` reverts to
  `HardwareSerial` on UART0 = **GPIO43/44, the GPS pins**, so all 89 `Serial.`
  call sites across 10 files must move to a `USBCDC` instance we own; and a VBUS
  misdetection costs the OTG flashing port on a board that only re-enumerates on
  a physical RST.

  That is real risk for an unknown number, and the number is probably small: the
  `usbd` task blocks (`tud_task()` → `tud_task_ext(UINT32_MAX)`), it does not
  spin, so the cost is PHY + clock, not CPU. **So measure first** — the
  `usbpower [sec]` console command samples the gauge with USB up, gates
  `PERIPH_USB_MODULE`, samples again, writes both to the diag log and reboots
  (`periph_module_disable` also asserts the module reset, so USB can't be
  resumed in place). Run it on battery with nothing mounted. If the answer is
  ≥10 mA the rename is worth it; if it's 2 mA, close this out for good.

## 3. CPU light sleep — the big lever, still unclaimed

> **Before touching `CONFIG_PM_ENABLE`, read
> [`pm-rebuild-baseline.md`](pm-rebuild-baseline.md)** — it records the exact
> known-good toolchain with content hashes (`tools/check-framework.sh` verifies
> them), explains why a *partial* SDK overlay is what broke `SD.begin()` on every
> local build last time, and gives the revert procedure.

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
