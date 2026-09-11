# Battery life and SD sleep investigation — 2026-09-10

Branch: `codex/battery-sd-sleep`, based on `19ea78c`.
Worktree: `/Users/raemond/Documents/tdisplay/.worktrees/battery-sd-sleep`.

**Current result:** the GPS receive guard now preserves complete 1 Hz GPS data
through CPU light sleep on this T5's CASIC/L76K module. The unplugged run recorded
**2,442 successful sleep calls**, **5,769 valid NMEA sentences**, **361 GGA and
360 RMC messages**, and **zero bad, truncated, or missing GGA/RMC epochs** in the
24 diagnostic windows containing sleep. UART error counters did not increase.
The phone remained on connection ID 1, idle at 300 ms. SD logging continued and
the card mounted successfully after USB returned.

The board runs `t5s3-painter-gps-wake`: fixed 240 MHz, MAIN_XTAL retained, GPIO44
receive-start wake, and no sleep during RX bursts or their 50 ms quiet guard.
CPU light sleep is enabled; idle auto-shutdown is OFF for this boot. The earlier
80–240 MHz DFS image remains an alternative with sleep disabled, not the current
board image. Two new battery samples were **97 mA**, compared with earlier
112.7 mA mean (DFS) and 116.7 mA mean (fixed-frequency awake) runs. These are
limited comparisons, not a controlled battery-life benchmark.

The receiver was searching for satellites, so this validates preservation of
its serial data, not position accuracy or navigation with a fix. Deep-sleep/button
wake cycles also remain unverified. All code and evidence stay in this worktree;
the main checkout and shared SDK remain unchanged. The original failed GPS
sleep runs and intermediate experiments below are historical evidence; see the
final GPS guard result at the end for the currently flashed behavior.

## What the existing evidence actually establishes

1. **Initial SD mounting happens before automatic light sleep is enabled.** In
   the original `main.cpp`, `ride_recorder::begin()` precedes
   `power_mgmt::begin()`. An initial mount failure therefore cannot, by itself,
   prove that this boot slept during the mount. A previous sleep/reset, card
   state, power settling, shared SPI pins, or the framework can still matter.

2. **There is historical evidence of SD and light sleep working together.**
   `investigations/sd-fix-verified-2026-08-05.log` contains a failed first mount,
   a successful retry, and subsequently `light_sleep=1 -> ESP_OK`.
   `investigations/battery-life.md` records the August 2 PM on/off experiment
   with mounts in both conditions. These are previous observations, not new
   validation of this branch.

3. **Several different states were called “no SD.”** `sdMounted()` is false
   when a USB host owns the card as well as when mounting fails. And
   Arduino `SDFS::begin()` tears down `_pdrv` on *any* mount failure, so
   `SD.cardType() == CARD_NONE` afterward cannot distinguish an absent card,
   failed initialization, or a filesystem failure. The console now points to
   real command/driver logs instead of calling every NONE a dead card.

4. **The previous recovery explanation cited the wrong driver.** ESP-IDF
   issue #14000 concerns CMD52 in its SDMMC/SDSPI initialization. This firmware
   uses Arduino `sd_diskio.cpp`, starts with CMD0, and supplies CMD0's CRC.
   Retaining a bounded CMD0 recovery sequence is reasonable; that issue is not
   proof of this firmware's root cause. CMD8 probe results now provide a second
   protocol observation. See [the upstream issue](https://github.com/espressif/esp-idf/issues/14000).

5. **The local framework really is PM-enabled, despite its stock-looking version.**
   It has a 223-byte `esp_pm_configure`, tickless idle, and the RTC slow clock
   selected for BLE. The verified stock copy has an 8-byte stub. Both package
   versions say `3.20014.231204`. The Arduino SD and SPI source hashes match
   between these two copies; the SDK archives/configuration differ. The two
   checked-in `battery-sd-*-framework.json` files record the specific content
   fingerprints, including the active `qio_opi` FreeRTOS archive.

## Changes and why they matter

### Light sleep and shared SPI

`power_mgmt::prepare()` now configures an awake baseline, creates both PM locks,
and acquires the setup guard **before** peripherals can start background work.
Only `begin()` subsequently enables automatic sleep. Failure to configure or
create a required lock keeps automatic sleep off. Previously, sleep was enabled
first and missing locks were silently tolerated. This closes an initialization
hole; it does not explain every historical SD failure.

The busy lock now serializes its cross-core acquire/release bookkeeping and
rejects an unmatched release without making the count negative. SPI startup and
free-space reporting use the same SD guard as other accesses. The SD/LoRa chip
selects retain their active GPIO configuration in light sleep.

The CPU remains at 240 MHz while awake; no frequency/PSRAM timing change is part
of this experiment. Automatic sleep must respect both RTOS and driver locks;
`pm clear` alone is not a residency measurement. This follows the version-matched
[ESP-IDF 4.4.6 PM model](https://docs.espressif.com/projects/esp-idf/en/v4.4.6/esp32s3/api-reference/system/power_management.html).

### Deep-sleep shutdown

After the final map/panel work finishes, shutdown takes the shared bus lock,
flushes diagnostics, unmounts SD, and **retains the lock until deep sleep**.
Previously it released the lock after `SD.end()`, allowing background SD/MSC/LoRa
work to start again during the remaining shutdown delays. The SD chip select is
held HIGH through deep sleep and its hold is released early on the next boot.
LoRa CS is not held through deep sleep because its supply is switched off.
Existing button wake configuration and GPS/LoRa power policy remain in place.

GPIO holds have to be released before reuse; see the matching
[ESP-IDF sleep documentation](https://docs.espressif.com/projects/esp-idf/en/v4.4.6/esp32s3/api-reference/system/sleep_modes.html).
No recovery procedure formats the card or cycles the board's battery supply.

### USB observability

The PM guard checks the BQ25896's live `VBUS_GD` status, independently of DTR/RTS,
and also respects USB card ownership. A failed charger read holds sleep off and
logs the fault/recovery. An attached host can therefore keep the console alive
without opening a serial monitor within the old 30-second window. The initial
window still exists. The read uses REG11 bit 7, matching the vendored charger
library and [TI's BQ25896 register definition](https://www.ti.com/lit/ds/symlink/bq25896.pdf).

The running Arduino 2.0.14 core starts USB from `app_main()` with our CDC-on-boot
flags. `USBMSC` registers its interface during static initialization. Old comments
claiming that a failed SD mount inherently prevents enumeration are not accurate
for this core. A pre-loop hang can still prevent the command console from running;
the RTC breadcrumb and flash snapshot address different parts of that problem.

## Evidence available when the card fails

- **Serial and daily SD logs:** mount attempt number, uptime, settle delay,
  clock, elapsed time, CMD0 R1, CMD8 R1/echo, chip-select/MISO levels, and success
  metadata. The existing bounded retry schedule remains; a lower transfer clock
  does not lower the library's already-400-kHz initialization clock.
- **Actual Arduino driver errors:** `sd_diskio.cpp`, `SD.cpp`, and `vfs_api.cpp`
  log messages are captured through a project-local linker wrapper. A separate
  bounded internal-RAM queue avoids taking the logger mutex from inside an SD
  write. The queue drains to Serial and the normal log outside that mutex.
  Overflow is reported. The stock framework's unused Insights ESP log wrappers
  are removed from this project's link flags to avoid a duplicate symbol; no
  installed package is patched.
- **Protected boot evidence:** first 8 KiB of boot log plus a 48 KiB recent
  pending buffer. If PSRAM allocation fails, the pending buffer falls back to
  8 KiB. Whole old lines are discarded on overflow, with a dropped-byte count.
  The protected boot copy is replayed to SD after a long outage.
- **Internal flash:** at most one 3 KiB NVS snapshot attempt per boot when a
  boot mount or log write fails, a diagnostic test fails, or shutdown has
  unpersisted lines. This survives removal of power. It is separate from the SD
  filesystem and is replayed into a later SD log; older snapshots are explicitly
  labeled. Allocation/NVS failure is still possible and is reported.
- **RTC breadcrumb:** last probe/mount/unmount/sleep stage, attempt, CMD0 result,
  and uptime. On a warm/deep-sleep reset it can identify where an interrupted
  operation stopped. RTC data is not a substitute for flash across loss of power.
- **Partial-write handling:** only bytes actually accepted by `File.write()`
  are consumed. Failed opens/short writes keep the remainder and back off for
  30 seconds. Arduino's `flush()`/`close()` return no status, so this cannot
  guarantee durability against a card that acknowledges writes then loses them.

`diag` dumps retained evidence over serial without consuming it. Lines already
successfully flushed are on the card; this command is not a mirror of the entire
historical SD log. Protected boot replay may deliberately repeat lines.

## Battery-life conclusion

The historical measurements show an expensive always-awake baseline. They do not
justify a promise of a particular new runtime:

| Historical state | Current magnitude | Nominal 1500 mAh / current |
|---|---:|---:|
| July ride/session average | 181–183 mA | 8.2–8.3 h |
| July awake idle | 162–167 mA | 9.0–9.3 h |
| August 2 light-sleep session mean | 130 mA | 11.5 h |

These are estimates from earlier notes, not a controlled comparison of the new
firmware. Usable capacity, temperature, GPS, display/frontlight, sensors, and
radio traffic affect the actual result.

**The baseline BLE policy is the major limit on riding savings.** The firmware holds
light sleep off while a phone, active sensor link, or sensor hunt requires it.
Historical supervision timeouts are why those protections exist. A longer BLE
interval alone already failed in the recorded experiment. The baseline build preserves
that policy; the separate main-crystal build below tests removing the phone hold. Reliable SD sleep is a prerequisite, but does not automatically
make a sensor-connected ride sleep. The GPS task's 50 ms poll is another ceiling
on sleep duration when that task is active.

Priorities after the SD tests: measure quiet idle first, then quantify the time
spent under each BLE hold. Improving sleep with connected BLE requires a separate
controller-clock/SDK investigation and verified board clock wiring. Do not infer
an ESP32 32-kHz crystal from the presence of the independent I2C RTC. Deep-sleep
current also needs remeasurement after the panel/rail shutdown fixes; the old
~2.5 mA estimate predates later changes.

## Maintaining the phone connection in light sleep

The user clarified that maintaining the phone connection is part of the target.
The earlier claim that this requires an external 32 kHz crystal was incomplete.
The **exact IDF 4.4.6 controller adapter already supports the main crystal staying
powered during automatic light sleep**. The relevant configuration is:

```ini
CONFIG_BT_CTRL_MODEM_SLEEP=y
CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1=y
CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL=y
CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP=y
# CONFIG_BT_CTRL_LPCLK_SEL_RTC_SLOW is not set
```

Espressif explicitly advises against the internal RC option for connected BLE.
See the matching [controller Kconfig](https://github.com/espressif/esp-idf/blob/v4.4.6/components/bt/controller/esp32c3/Kconfig.in)
and [adapter implementation](https://github.com/espressif/esp-idf/blob/v4.4.6/components/bt/controller/esp32c3/bt.c).
The C3 directory also contains the S3 adapter. Keeping the crystal powered costs
some sleep current, which must be measured, but avoids requiring a separate
32 kHz crystal. Automatic light sleep can maintain the connection; deep-sleep
shutdown cannot. This follows [Espressif's sleep-mode distinction](https://docs.espressif.com/projects/esp-idf/en/v4.4.6/esp32s3/api-reference/system/sleep_modes.html).

`t5s3-painter-ble-xtal` is an isolated environment using an unmodified, licensed
copy of that matching controller adapter, compiled with the above options.
It corrects the caller's sleep-clock field at controller initialization too.
A post-link check verifies the upstream SHA-256 and that the old precompiled
`libbt.a(bt.c.obj)` was not linked. No installed SDK files are changed. The
baseline environment and its RC controller remain available for comparison.

The main-crystal build permits phone-connected sleep at any negotiated interval
once the replacement controller reports enabled. It defaults the existing
`sleepexp` switch ON and retains its three-supervision-timeout fallback for the
boot. `sleepexp off` restores the phone hold and controller-awake policy. Sensor
links and hunting retain their existing holds pending their own tests.

`sdtest status` now retrieves the latest test result from RAM even after the
log flushed to SD. `phone_sleep_pass` only counts a successful SD operation
following sleep when the phone was connected at both sample boundaries with the
same connection ID; a disconnect/reconnect changes that ID. Test results survive
serial reconnects but not a board reset. Daily logs remain the durable history.

Build, link verification, and host policy tests pass. The crystal build is
flashed and booted: first-attempt SD mount in 12 ms, controller ready, phone
connected with its application sleep hold released. Hardware sleep/connection
acceptance remains pending. For this environment, keep the companion app connected and unplug only
USB during the battery test. Require positive `phone_sleep_pass`, no SD failures,
no unexpected phone reconnects, and no fallback latch before claiming success.

## First unplugged result: stale serial state prevented CPU sleep

The main-crystal image's 15:00:07 test completed with **115 passes, zero failures,
zero skips, but zero CPU sleep calls**. The user unplugged USB around 15:03:33 and
reconnected around 15:18:07. The SD log confirms VBUS disappeared and the gauge
reported discharge, while every PM window still reported `holders=serial+prlx`.
`prlx` denotes a connected phone allowed to sleep; **the cached serial flag was
the blocker**. No phone disconnect was logged during this battery interval.
See `battery-sd-unplugged-2026-09-10.log` (local-only evidence).

Seven battery samples averaged **114.4 mA discharge** (113–115 mA), with GPS
searching and the phone connected. This is a measurement of this run with
Bluetooth modem sleep but CPU light sleep blocked. It is not a controlled
before/after comparison and does not establish a runtime improvement.

Arduino's native CDC `operator bool()` returns cached connection state, which
can remain true after cable removal. The guard now ignores that cached flag
when the charger's live status positively establishes that VBUS is absent.
An unreadable charger still holds sleep off. `pm USB` transition logs report
VBUS, validity, raw CDC state, and the resulting serial hold independently.
Host fault tests cover stale CDC on battery and unreadable VBUS with stale CDC.

While retrieving this result via USB mass storage, the board reset with a task
watchdog. The card mounted successfully again. USB drive mode was restored to
its original OFF setting. That MSC reset is an unresolved separate finding;
this branch does not claim to fix it. QuntisBar was paused during serial access
and resumed afterward.

To retrieve evidence without MSC, `diag sd [bytes]` now reads the daily log
through serial (default last 128 KiB, maximum 256 KiB). Chunks carry offsets and
hex encoding so unrelated serial logs cannot silently corrupt the download.
`tools/power_sd_decode.py` requires contiguous bytes and a complete end marker;
missing SD, active recording, and host ownership refuse the SD read while plain
`diag` remains available. The first verified transfer recovered **87,198 bytes**.

```sh
python3 tools/power_capture.py --output .pio/sd-capture.log --seconds 45 \
  --command 'diag sd 262144'
python3 tools/power_sd_decode.py .pio/sd-capture.log .pio/sd-tail.log
```

The stale-CDC fix and log reader were flashed at 15:25. SD mounted on the first
attempt, and a repeat test was armed at 15:27:19 with auto-shutdown OFF, USB drive
OFF, and the phone connected. The result of that repeat is below; the first run must not be counted as sleep validation.

## Repeat result: connection survives sleep; display polling limits sleep time

The repeat ran on battery from about 15:28:51 to 15:35:27. VBUS and serial holds
cleared, and the phone remained on connection ID 1. The complete test reported:

```text
pass=115 fail=0 skipped=0 sleep_calls=8 after_sleep_pass=7 phone_sleep_pass=7
```

There were no recorded phone disconnects or supervision-timeout fallback. SD was
still mounted after reconnect. This establishes a small number of successful
sleep/wake/SD operations with the phone connected. The sleep calls totaled just
33 ms, including overhead, over roughly 396 seconds on battery—under 0.01% of
that interval. Three current samples averaged 116.7 mA (115–118 mA), so useful
CPU-sleep savings have not yet been demonstrated. Evidence:
`battery-sd-phone-sleep-2026-09-10.log` (local-only evidence).

The remaining idle blocker found in source is the display dependency's
`_paint_task_body()`: while `paintStage == 0`, it calls `vTaskDelay(1)` repeatedly.
With the SDK's 1 kHz tick and three-tick minimum idle period, that worker normally
prevents the required idle window. The application task periods are much longer.

`tools/epd_idle_patch.py` replaces that specific idle poll with a dedicated binary
work semaphore in the project's private EPD Painter dependency copy (examined
commit `6335a149c5a9b3be4150880e0be64ef2529ba62a`). All six asynchronous submission
paths signal it after publishing the frame. Its lifecycle includes allocation
failure handling and deletion after the worker stops. It does not reuse the task
notification mechanism or alter active rendering timing. The build hook checks
its expected source structure and refuses partial patches; installed/shared
libraries remain untouched.

The idle-loop host test covers work submitted before waiting, an old semaphore
token at the next idle transition, a wake without new work, and no polling while
blocked. The main-crystal firmware builds with the patch. The display change was flashed at 15:43: SD mounted in 12 ms, all tasks
started, the UI command parser responded, and the phone reconnected. A new
600-second test was armed at 15:44:43 with auto-shutdown and USB drive exposure
OFF. The result is below.

### Display fix battery result and GPS regression

Battery-only interval: 15:48:31–15:50:51 local (about 140 seconds). Final:

```text
pass=111 fail=0 skipped=0 sleep_calls=6904 after_sleep_pass=25 phone_sleep_pass=25
successful_calls=6904 rejected=0 time_in_calls_ms=95829
```

Phone connection ID stayed 1 across all checks; no recorded disconnect or
supervision-timeout fallback. SD remounted in 12 ms on USB removal and 13 ms on
reconnection; it remained accessible afterward. Calls occupied about 68% of the
battery interval, including entry/exit overhead. The complete battery-only PM
window recorded 40,912 ms in calls over 60,473 ms (67.7%).

The single discharge sample at 15:49:44 was **87 mA** with GPS searching, versus
115–118 mA in the previous run. This suggests improvement but is not a controlled
capacity/runtime measurement, and GPS data loss makes it unsuitable as a final
power comparison. GPS checksum counters rose from `4838/1` before unplugging to
`5109/733` after reconnecting; bad checksums then stopped increasing. Valid NMEA
throughput also collapsed during sleep.

Version-matched [IDF sleep GPIO setup](https://github.com/espressif/esp-idf/blob/v4.4.6/components/esp_hw_support/sleep_gpio.c)
can isolate sleep pins and enable automatic switching. A pin-retention hypothesis
was tested at boot: GPS RX/TX mux registers were both `0x00001b00` before and
after `gpio_sleep_sel_dis`; `SLP_SEL` was already clear. Thus the explicit pin
exception is defensive and **does not establish a fix for this regression**.
Arduino's S3 UART selects XTAL, and the BLE profile requests that clock remain
powered. RX through sleep still needs hardware verification; a retained crystal
alone does not prove the whole receive path runs.

The next image counts UART FIFO overflow, ring-buffer overflow, framing, and
parity events without logging from the serial event task. The GPS task logs
those counters, clock/mux registers, and bytes/good/bad/sleep deltas. If a window
contains at least 20 bad sentences, more bad than good, and successful sleep
calls, it requests `pm off` and checkpoints evidence once. This stops a recurrence
of the observed severe corruption without a reset; the user can explicitly
re-enable PM for further diagnosis. It does not detect every possible data loss.
The startup pin probe at 15:59 also mounted SD in 12 ms and reconnected the phone.

Simply enabling UART wakeup would not ensure lossless NMEA:
[IDF documents lost trigger characters](https://docs.espressif.com/projects/esp-idf/en/v4.4.6/esp32s3/api-reference/system/sleep_modes.html#uart-wakeup-light-sleep-only).
The [S3 TRM](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf)
separates UART core and APB/FIFO clock domains and supports UART wake on UART0/1;
the current GPS uses UART2. Source comments that treated a 50 ms poll alone as
a guarantee of lossless reception were removed.

The first serial SD transfer failed the decoder's continuity check. Repeating it
with the competing QuntisBar process temporarily paused recovered a complete
32,768-byte tail; QuntisBar was resumed. Evidence:
`battery-sd-display-sleep-2026-09-10.log` (local-only evidence).

## Repeatable hardware procedure

Use the *same card, firmware, battery level range, frontlight setting, GPS state,
and sensor/phone conditions* for both legs. Eject/disable the USB drive before a
battery run so the card returns to firmware ownership. USB power intentionally
suppresses light sleep, so testing with a serial cable attached is insufficient.

1. Flash the worktree build through the existing `tools/flash.py` workflow.
   On the console run `sd`, `pm`, and `diag`. Confirm the mount logs, build
   identity, and `pm available=1` on the PM image.
2. Run `autosleep off` to disable the idle shutdown for this boot. This does
   not disable CPU light sleep or manual power-off. `autosleep` reports the
   current setting; `autosleep on` re-enables it with a fresh 10-minute window.
   Every reset restores the default ON. Then run `pm off` and `sdtest 600`. Unplug USB within 15 seconds. Keep the device
   awake for the test. It creates a uniquely named 512-byte temporary file under
   `/logs`, closes it, reopens/compares it, and removes it every five seconds.
   It skips checks while recording or while the card is unavailable/host-owned.
3. Reconnect and retrieve logs. Run `pm on`, then repeat `sdtest 600` with USB
   unplugged. Wait out boot grace and sensor hunting. Keep the phone connected
   for the main-XTAL build; the RC baseline holds sleep while connected.
   **Require `after_sleep_pass > 0`**, preferably at least 100 passes with zero
   failures. A passing run with zero preceding sleep calls only tests awake SD.
4. Repeat at least 20 planned deep-sleep/button-wake cycles and verify a mount
   each time. Include a session with a ride saved before shutdown. Confirm
   prior RTC stage 7 and the expected button wake reason.
5. Boot with the card removed; retrieve `diag`. Reinsert it and wait for the
   existing background mount retry; confirm failure evidence reaches `/logs`.
   Also power-cycle after a missing-card boot to verify the flash snapshot.
6. Exercise USB ownership separately: host owns the card, eject, unplug,
   reconnect, host suspend. Logs must distinguish ownership from mount failure.
   Do not confuse the pre-existing five-minute host-idle reclaim heuristic with
   physical removal; comprehensive MSC arbitration is outside this patch.
7. For current measurements, compare equal battery-only windows after settling.
   Correlate the existing signed `battery:` samples with `pm window` counts and
   hold reasons. `sleep_call_ms` measures time inside successful light-sleep
   calls, **including entry/exit overhead**, not a precision power-domain meter.
   It must not be interpreted as exact deep-sleep residency.

Retrieve evidence through a single stable CDC connection (pyserial is included
with PlatformIO):

```sh
python3 tools/power_capture.py --output .pio/after-sleep.log --seconds 30
```

The default commands are `sd`, `pm`, `diag`; use repeated `--command` arguments
for a particular sequence. The helper rejects the ROM port, opens once, keeps
DTR/RTS stable, and does not repeatedly reconnect after a USB drop. These controls
avoid repeating the reset-handshake problems documented in the older notes.

## Connected-board results

The PM image was flashed on September 10. All uploaded regions passed esptool's
hash verification. The post-upload helper initially selected an unrelated global
esptool installation and failed on a missing `intelhex` dependency after the flash
had completed. `tools/flash.py` now selects the pinned esptool from the selected
`PLATFORMIO_CORE_DIR` and runs it in a fresh interpreter. Its finish command
successfully cleared the ROM download latch and reset into the application.

Selected serial evidence is preserved in
`battery-sd-hardware-2026-09-10.log` (local-only evidence).

- Boot probe: CMD0 `0x01`, CMD8 `0x01`, echo `0x000001aa`; first mount at
  4 MHz completed in 12 ms. The card reports SDHC, 30,436 MB.
- PM initialization succeeded with both guards established before peripherals.
- `autosleep on`, `autosleep off`, and the status query worked on hardware.
  The board is left with idle shutdown OFF for this boot.
- Connected 30-second test: `pass=3 fail=0 skipped=0 sleep_calls=0
  after_sleep_pass=0`. USB and phone holds correctly prevented light sleep.
- The earlier RC-build battery test was armed at 14:33:19 local time; its
  unplugged result was not retrieved before moving to the phone-connected target.
- The main-crystal build was flashed successfully at 14:58. Another first-attempt
  SD mount completed in 12 ms; `controller_ready=1`. At 15:00:07 a new 600-second
  test was armed with the phone connected; only USB should be removed for this
  test. The first battery-only result is recorded above; a stale serial flag prevented CPU sleep.
- USB drive exposure was OFF at the start of testing.
- Two crystal-build upload attempts failed (serial disconnect, then flash hash
  mismatch). `lsof` showed QuntisBar holding the exact board ROM serial port.
  After temporarily pausing that process, the entire upload verified and the app
  booted successfully. The stray `i` console characters also disappeared during
  the exclusive capture. QuntisBar was resumed afterward. This is evidence of
  host serial interference, not proof that it caused historical SD failures.

## Build and test evidence

All packages, generated files, and firmware artifacts used here are inside this
worktree. The shared `~/.platformio` packages were read/copied, never overlaid.

```sh
# Use the private PM package copy already prepared in this worktree.
PLATFORMIO_CORE_DIR="$PWD/.pio/core" pio run -e t5s3-painter
PLATFORMIO_CORE_DIR="$PWD/.pio/core" pio run -e t5s3-painter-pm-awake

# Separate verified stock SDK, separate build output.
PLATFORMIO_CORE_DIR="$PWD/.pio/core-stock" \
  PLATFORMIO_BUILD_DIR="$PWD/.pio/build-stock" pio run -e t5s3-painter

# Phone-connected light-sleep experiment, same private PM SDK.
PLATFORMIO_CORE_DIR="$PWD/.pio/core" pio run -e t5s3-painter-ble-xtal

python3 tools/power_test/run_tests.py
python3 tools/power_framework_report.py --core-dir .pio/core
```

- PM sleep-on, PM initially-awake, stock-framework, and main-crystal BLE builds: passed.
  `battery-sd-firmware-images.json` records their sizes and SHA-256 hashes.
- Stock SDK baseline check (`tools/check-framework.sh --fast`): passed, including
  the expected 8-byte stub and recorded config/build-script/archive hashes.
- Host tests compile the actual PM/logger implementations with hardware includes
  replaced by mocks. AddressSanitizer/UBSan checks pass for unavailable PM, first
  and second lock allocation failure, nested/unbalanced bus guards, VBUS present
  or unreadable, runtime sleep disable, zero-size status output, absent SD,
  short writes, retry backoff, failed opens, overflow/boot preservation, and the
  one-checkpoint-per-boot limit, low-memory fallback, and deferred driver logging/queue overflow.
  Added main-crystal policy checks cover a fast connected phone, timeout/switch
  fallback, an unready controller despite a long interval, and retained sensor-hunt protection.
- Link map confirms the IDF PM implementation calls our sleep wrapper in IRAM,
  and the SD library calls the diagnostic wrapper. The counter does not rely on
  a framework rebuild with PM profiling enabled.
- Existing USB-mode redefinition warnings remain. The unchanged epdiy fallback
  environment was not part of the three-image validation.

Full hardware acceptance, controlled battery-life measurements, GPS RX through sleep,
and arbitrary power-loss recovery remain unverified. Keep the build variants and their logs separate; do not
format a card to make a failed test disappear.

### GPS diagnostic image, 16:04 boot

The UART error/fallback image flashed and verified successfully. SD mounted on
the first attempt in 12 ms, the phone connected at 30 ms, and auto-shutdown was
disabled for this boot. UART clock register `0x03701000` selects XTAL with RX/TX
clocks enabled; both GPS mux registers still have `SLP_SEL` clear. Startup
counters were FIFO=4, buffer=1, frame=0, parity=0; use deltas from the settled
window, not those startup totals, to judge the next battery test. All four build
variants and the existing host checks passed after this diagnostic change.

A 600-second repeat was armed at 16:05:16 local, with phone sleep ARMED and
auto-shutdown OFF. The result is recorded below.

### GPS error classification and fallback result, 16:11 battery run

Final SD result: **114 passed, zero failed, zero skipped; 1,047 successful sleep
calls; five checks after sleep on the same phone connection**. Successful calls
totaled 14.642 s with no rejected calls. SD was still mounted after USB returned.
The phone remained on connection ID 1 throughout.

VBUS went absent at 16:11:41. In the fully affected GPS diagnostic window,
reception fell to 3,917 bytes / 20 good / 83 bad sentences, versus roughly
9,000 bytes / 225 good / zero bad before unplugging. UART FIFO=4, buffer=1,
frame=0, parity=0 remained unchanged across the transition; clock/mux registers
also remained unchanged. This rules out a *reported* FIFO/ring overflow or
framing-error storm in this run. The evidence is consistent with missing input
while the receive path stops during sleep; it does not locate the exact hardware
gate by itself.

At 16:11:56 the safeguard requested PM off and saved the bounded flash snapshot;
at 16:11:57 PM confirmed light sleep disabled. After the transition window,
reception recovered to 225–226 good sentences and zero bad per 15 seconds,
continuing on battery with the phone connected. The later battery sample was
116 mA with sleep disabled. This verifies the automatic corruption fallback and
log retention, not a GPS-through-sleep fix. Evidence:
`battery-sd-gps-fallback-2026-09-10.log` (local-only evidence).

### CPU-frequency-scaling comparison

A separate `t5s3-painter-ble-dfs` profile starts with light sleep disabled and
allows CPU frequency to drop to 80 MHz when idle, rising to 240 MHz for work.
APB stays at 80 MHz across this range, avoiding the peripheral clock change
associated with lower CPU minima. Initialization remains at 240/240 MHz until
peripherals and PM guards are ready. The main-XTAL Bluetooth modem-sleep policy
is retained. Runtime `pm on/off` preserves the selected minimum, and `pm` logs
that frequency policy. This profile is an alternative battery experiment while
GPS reception requires the receive path to remain awake; it is not counted as
successful light-sleep validation. Its battery/GPS/SD result is recorded below.

Host tests now verify fixed-frequency initialization, the selected minimum at
begin and runtime toggles, and the same fail-closed missing-SDK/lock behavior for
the 80 MHz profile.

The DFS image flashed at 16:20 and reported `min=80 max=240MHz APB=80MHz
light_sleep=0 -> ESP_OK`. SD mounted in 12 ms; GPS passed 225 sentences with
zero bad checksums in the first complete 15-second window. The phone connected
at 16:20:40 with a 30 ms interval. Auto-shutdown is OFF for this boot. All five
build variants and the expanded host checks passed; `git diff --check` is clean.
The new environment uses copies of the already-tested private dependencies.
Battery consumption under this profile is recorded below.

The next 600-second SD/battery comparison was armed at 16:21:30, with
phone connected and CPU light sleep OFF. The unplugged measurement is below.

### CPU-scaling battery result, 16:24–16:39

The board ran on battery from approximately 16:24:56 until 16:39:01 local (about
14 minutes; wall-clock adjustments mean these are approximate durations).
Final test result: `pass=115 fail=0 skipped=0 sleep_calls=0`. Of these, **77**
write/close/reopen/compare/remove checks occurred in the unplugged interval,
all with `phone_same_link=1 phone_id=1`. The phone did not reconnect during that
interval. SD remounted in 12 ms on both USB removal and return and remained
mounted when queried. Zero light-sleep calls is expected for this profile.

All 55 captured GPS windows in the battery interval recorded zero bad
checksums; live status exceeded 17,000 valid sentences since boot with zero bad.
UART FIFO/buffer/framing/parity counters stayed at their startup values. GPS
remained in acquisition/search, so this establishes clean serial reception,
not fix accuracy, navigation behavior, or ride-recording acceptance.

Discharge samples (mA magnitude): **114, 109, 114, 114, 112, 114, 112**.
Mean **112.7 mA**, median **114 mA**. Compared with the earlier fixed-240 MHz
mean of 116.7 mA, the observed difference is about 3.4%, too small and too weakly
controlled to claim a substantial battery-life gain. Firmware policy is verified;
we have not directly measured CPU frequency residency. The 87 mA light-sleep
sample is not a usable alternative with continuous GPS because that run lost data.

The board is left on the DFS image, CPU light sleep OFF, idle auto-shutdown OFF
for this boot, phone connected. No additional unplug cycle is armed. Evidence:
`battery-sd-dfs-result-2026-09-10.log` (local-only evidence).

A proposed next power policy is to pause GPS only while idle, not recording,
and not navigating, allowing phone-connected light sleep then. The user's
preference is pending; no such behavior has been implemented or enabled.
The board shares the GPS/LoRa power rail, so any implementation must preserve
mesh-radio behavior rather than simply cutting that shared rail.

### Media-control overlap review

The user flagged Apple Music and basic BLE controls as a possible duplication.
In this worktree there are two paths, not a separate HID/AVRCP implementation:

- `ams_client.cpp` uses the iPhone's Apple Media Service over the existing BLE
  connection. It sends controls to the active media player and subscribes to
  metadata/playback changes. `ams::tick()` returns immediately after setup
  unless new setup work is queued; it does not poll metadata continuously.
- `companion-ios/Sources/MediaRemote.swift` uses `systemMusicPlayer`, sends
  Apple-Music-specific metadata and 300×300 grayscale artwork, and handles
  fallback commands when AMS fails. It is enabled when a music page exists,
  responds to playback/item notifications, and sends artwork on item changes.
  One image is 90,000 payload bytes before BLE framing. Firmware ignores the
  app's metadata for 20 seconds after an AMS update but still receives its BLE
  writes; metadata-source arbitration is not transport deduplication.

The source supports retaining AMS for active-player controls and making the
companion Apple Music metadata/artwork feed optional or suppressing duplicates
while AMS is available. Removing AMS instead would remove the system-wide
control path. Neither path currently calls the bulk-transfer governor hook;
the known fast-link cause in the indoor tests was its GPS-fix requirement.
There is no measured media-specific energy attribution yet. No controls or
artwork behavior were removed during this review.

Primary protocol reference: [Apple Media Service](https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleMediaService_Reference/Specification/Specification.html).


### Apple Music removal and the 30 ms interval

At the user's request, the iOS companion's Apple-Music-only `MediaRemote`
implementation, Music permission string, custom-media subscription, artwork
uploads and command relay have been removed. Direct firmware AMS controls and
now-playing metadata remain. The custom media characteristic remains for the
Android companion, which uses it for Android media sessions. The updated iOS
simulator app builds successfully; it has not been installed on the phone.
Consequently the currently installed companion can still send the old traffic.

The previous governor required **ten seconds of GPS fix** as well as eight
seconds without bulk traffic before requesting a longer interval. All recent
indoor battery runs were searching for GPS, so the device itself continued to
request 15–30 ms. A one-second application status update does not imply a
one-second BLE connection interval: the latter schedules radio connection events
between payload updates. Apple Music was not the cause of this GPS-dependent
policy.

The replacement policy has no GPS dependency. It allows 15 seconds for initial
connection setup, then requests **150–300 ms, latency 0, supervision timeout
4 seconds** when bulk traffic has been quiet for at least eight seconds.
Transfers request **15–30 ms** again. Long outgoing streams service the governor
while sending, and active OTA prevents relaxation. Three-second fast-request
and 15-second relaxed-request spacing avoid request churn. If the measured
interval remains outside the requested range, retries are limited to three
attempts per mode, separated by 60 seconds. An accepted interval clears the
retry budget; a later central override is therefore observed and retried.

Both ranges satisfy Apple's published interval/latency/timeout constraints:
[QA1931](https://developer.apple.com/library/archive/qa/qa1931/_index.html).
The phone remains responsible for choosing the actual interval. These changes
reduce requested radio event frequency; there is no new measured battery-life
gain until hardware comparison has been completed.

New `bleinterval [status|fast]` serial commands report measured parameters or
request a transient fast period, then return to the idle policy. Diagnostics
include local request return codes, negotiated-parameter callbacks, actual versus
desired intervals, latency, timeout, retry count and phone connection ID. They
use the existing serial/SD logger and its bounded RAM retention if SD is absent.
Logs recur on changes/requests and once per minute while connected, rather than
on every connection event. Host ASan/UBSan checks cover initial grace, quiet
periods, rejected/accepted/overridden intervals, bounded retries, transfers,
active OTA, reconnect reset and timer wrap.

Hardware verification was initially deferred because the board's USB CDC port
was absent when the build completed. The subsequent flash and measured interval
acceptance are recorded below.

Validation completed for the interval change: all five firmware environments
(DFS, main-XTAL light sleep, fixed-frequency PM, initially-awake PM, and stock
SDK) built successfully. The iOS simulator build and all host diagnostic/policy
checks passed. Build logs are in `.pio/ble-interval-*-build*.log` and
`.pio/ios-no-apple-music-build.log`; current binary hashes are in
`battery-sd-firmware-images.json`. The earlier measured DFS binary hash is
retained under `measured-dfs-16-24` to keep its battery evidence attributable.


### BLE interval hardware validation, 17:36–17:46

Flashed the updated `t5s3-painter-ble-dfs` worktree image. SD mounted on the first
attempt in **12 ms**. The phone connected at 17:36:43 with a 30 ms interval and
AMS initialized at 17:36:50. At 17:37:04 the idle policy queued 150–300 ms;
at 17:37:05 the negotiated callback and actual-state read both reported
**300 ms, peripheral latency 0, supervision timeout 4,000 ms**.

A serial `bleinterval fast` request at 17:44:38 produced a measured **30 ms**
interval at 17:44:41. The governor requested idle again at 17:44:53 and measured
**300 ms** at 17:44:54. All captures retain **phone connection ID 1**, with no
recorded disconnect. Other traffic during this run also triggered fast periods,
followed by successful relaxation. Thus 300 ms is the idle state, not a forced
interval during all app activity. GPS obtained a fix during this run; removal
of the GPS gate is additionally verified by the GPS-independent policy tests.

The short SD test completed **three passes, zero failures, zero skips**, each
writing 512 bytes, closing, reopening, comparing and removing its temporary
file on the same phone link. A complete 32,768-byte SD log tail was read back
over serial and decoded with contiguous-offset/end validation. It contains the
actual negotiation records and the completed SD result, establishing that these
new diagnostics reached SD as well as serial. Existing host fault tests cover
retention when SD is unavailable; this run had no mount failure.

Evidence: `battery-sd-ble-interval-2026-09-10.log` (local-only evidence).
The binary hash is under `pm-ble-dfs` in `battery-sd-firmware-images.json`.
The board remains on this firmware with per-boot auto-shutdown OFF, DFS enabled,
and CPU light sleep OFF. No battery-current gain is claimed from this USB-powered
check, and it does not resolve GPS reception through CPU light sleep or validate
deep-sleep/button-wake cycles. The iOS Apple Music removal is built but has not
been installed on the phone; direct AMS initialization is confirmed here.


### GPS receive-start wake candidate (hardware validation pending)

The ESP32-S3 TRM §26.4.1 places UART FIFOs in the APB clock domain while the
receiver core has its own selectable clock. Keeping the main crystal and the
RX/TX pin configuration alive does not establish lossless FIFO reception during
light sleep. IDF 4.4.6 only exposes UART0/1 wake, while this board uses UART2;
ordinary UART wake also discards triggering characters. References:
[ESP32-S3 TRM](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf),
[Espressif sleep documentation](https://docs.espressif.com/projects/esp-idf/en/v5.0/esp32s3/api-reference/system/sleep_modes.html).

The new **experimental** `t5s3-painter-gps-wake` profile retains MAIN_XTAL and
80–240 MHz DFS, enables light sleep, and installs a GPIO44 low-level wake source
on the existing UART RX pin. Before sleep it checks the physical RX level, UART
RX state machine, and FIFO. An in-progress receive defers sleep. A GPIO wake
holds off further sleep until the GPS task has drained the data and observed
50 ms of quiet. It does not power down GPS or the shared GPS/LoRa rail, change
baud rate, or change the receiver's NMEA output configuration.

The intended mechanism is waking at the start bit, before a 9600-baud character
finishes. **It has not yet been shown that the first character survives on this
hardware.** Nor has this strategy been validated for the alternate 38400-baud
u-blox module. At high NMEA traffic volumes there may be little quiet time left
for sleep; successful reception alone will not establish useful power savings.

The guard uses IDF's `esp_pm_register_skip_light_sleep_callback` and a final
check in the IRAM sleep wrapper. It performs no PM-lock calls while holding its
own critical section: IDF already owns its switch lock at sleep entry, so a
second cross-core lock order would risk deadlock. Callbacks/internal state are
IRAM/DRAM. The source is restricted to the audited S3/IDF 4.4.6/MAIN_XTAL build.
Failed callback or wake-source setup retains the existing no-sleep bus guard.

An independent raw NMEA audit now logs complete GGA/RMC counts, malformed or
truncated sentences, and gaps between their UTC-second epochs. This catches
whole missing sentences that TinyGPS checksum counters alone can miss.
Duplicate epochs and day rollover are handled. In the experimental profile,
15-second diagnostic windows continue after a GPS fix. After a normal 1 Hz
window establishes reception, substantial missing position/time messages or
checksum loss during sleep requests PM off and saves the existing diagnostic
checkpoint. This remains a fallback, not evidence of successful reception.

Host tests exercise production guard logic for low RX at entry, a frame in
progress with RX high, GPIO wake already deasserted, nonempty FIFO, received
bytes, quiet-time release, repeated callbacks, and all three setup failures.
The audit tests cover checksums, truncation/overflow, missing epochs, duplicates,
invalid UTC and midnight rollover. The expanded suite passes with ASan/UBSan.

The T5 disappeared from USB before this image could be flashed. The observed
USB serial devices were an LG monitor and XIAO nRF52840, which were not modified.
The last known T5 image is still the measured BLE-interval DFS build with CPU
light sleep OFF. To finish: flash this candidate, establish awake GGA/RMC rates,
arm an SD check, unplug USB while retaining the phone link, and verify successful
sleep calls together with complete 1 Hz GPS epochs, no new UART errors, SD
integrity, and actual battery-current samples. Only then enable it in the normal
profile or claim the GPS issue resolved.


### GPS guard hardware setup, 18:56–19:01

The initial guard image was flashed after the T5 returned. Awake reception
produced complete 15-second windows of **15 GGA, 15 RMC, no bad/truncated
sentences and no missing epochs**. UART error counters stayed at startup
values. SD mounted in 13 ms. This proves the guard does not corrupt the awake
stream; USB prevented all actual CPU sleep.

Three phone supervision timeouts occurred while the image used 80–240 MHz DFS
and automatic light sleep was configured, despite **zero successful sleep
calls**. The connected-sleep fallback disabled the experiment. A subsequent
PM-off comparison re-armed the phone modem-sleep experiment and remained
connected through the captured 35-second run. This is not enough to attribute
the timeouts to frequency scaling, the receive guard, or RF conditions.
Evidence: `battery-sd-gps-rx-guard-awake-2026-09-10.log`.

To isolate the GPS change, `t5s3-painter-gps-wake` now uses the **fixed 240 MHz
MAIN_XTAL** profile that previously maintained the phone through 6,904 measured
sleep calls. It was reflashed at 19:01, with SD again mounting in 13 ms. The
80 MHz image's hash remains under `gps-rx-guard-dfs-awake-18-56`. The current
candidate still requires an unplugged test; no claim of GPS-through-sleep or
battery-life improvement follows from these awake captures.


At 19:03:17 the fixed-240 MHz guard image armed a 180-second SD check. Awake
windows again had 15 GGA/15 RMC with no bad/truncated sentences or missing
epochs. Phone connection ID 1 remained connected at 300 ms; the PM report was
`sleep_configured=1` with only USB/serial holders and the informational `prlx`
flag. The user was asked to unplug for about two minutes while keeping the phone
connected, then reconnect USB for retained-log retrieval. Results follow when
available; arming a test is not evidence of sleep.


### GPS receive-guard result, 19:25–19:31

**GPS data survived actual light sleep.** USB power was absent from 19:25:15
until 19:31:03 (about 5 minutes 48 seconds; GPS can adjust wall-clock time).
The final counters were **2,442 successful sleep calls**, nine rejected calls,
and **46.211 seconds inside successful sleep calls, including entry/exit
overhead**. The GPS corruption/missing-epoch fallback did not trigger; PM
remained configured ON. Guard bursts and releases continued to advance.

In the **24 GPS diagnostic windows containing sleep**, the raw audit recorded
**361 complete GGA and 360 complete RMC messages**, zero checksum failures,
zero truncated sentences and zero missing UTC-second epochs for either type.
TinyGPS separately recorded **248,576 bytes and 5,769 valid sentences**, with
zero new bad checksums. The first/last windows overlap USB transitions; the
full interior windows likewise retained complete 1 Hz position/time messages.
UART FIFO/buffer/frame/parity counters remained **3/1/0/0**, their startup values.
The receiver was searching, so this establishes intact GPS serial reception,
not a satellite fix, position accuracy, or navigation acceptance.

The phone remained on **connection ID 1**, with idle interval **300 ms** and no
disconnect in the retrieved evidence. A 32,768-byte SD tail was retrieved and
validated for contiguous offsets and a complete transfer. It contains the
sleep-period GPS/PM/battery records, demonstrating SD logging through the run.
SD mounted in **12 ms** at USB removal, **13 ms** on return, and **12 ms** during
the subsequent ownership transition. It was mounted when queried afterward.

The 180-second SD stress test armed at 19:03 had finished before USB was removed:
its **33 passes happened without sleep**, and must not be counted as stress
checks during the battery run. After reading the results, a fresh 30-second
post-wake test completed **three write/close/reopen/compare/remove passes, zero
failures, zero skips**, on the same phone link. Thus the new evidence verifies
persisted logging during sleep and explicit SD integrity after sleep, with the
separate earlier branch tests covering stress writes between sleep cycles.

Battery samples at 19:26:24 and 19:28:54 were both **97 mA discharge**. These
are lower than the earlier 112.7 mA mean DFS-awake and 116.7 mA fixed-awake
samples, but two readings under different runs do not establish a controlled
battery-life gain. No battery-runtime estimate is inferred.

Evidence: `battery-sd-gps-rx-guard-result-2026-09-10.log` (local-only evidence).
The flashed binary is `pm-gps-rx-wake` in `battery-sd-firmware-images.json`.
Use the explicit `t5s3-painter-gps-wake` environment for this validated behavior;
the shipping/CI default has not been changed or published. In this worktree:

```sh
PLATFORMIO_CORE_DIR="$PWD/.pio/core" pio run -e t5s3-painter-gps-wake
PLATFORMIO_CORE_DIR="$PWD/.pio/core" pio run -e t5s3-painter-gps-wake -t upload
```

The board is left on this fixed-240 MHz MAIN_XTAL + GPS guard image, light sleep
ON and idle auto-shutdown OFF for the current boot. The post-wake test is
finished; no additional unplug cycle is armed. The alternate u-blox module,
80 MHz + light-sleep combination, sustained navigation with a fix, and
full deep-sleep/button-wake cycles have not been accepted by this test.

Raw logs, crash dumps, and exported investigation results are retained locally in ignored paths. They must not be committed: diagnostic breadcrumbs and captured memory can contain GPS coordinates.
