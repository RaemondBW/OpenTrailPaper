# Battery follow-up: GPS output, event-driven receive, and sensor sleep

Worktree: `.worktrees/battery-sd-sleep`, branch `codex/battery-sd-sleep`.
Base: `3a16c5d`.

## Scope and current state

The user selected three further optimizations: reduce redundant GPS output,
replace periodic GPS polling/shorten the receive guard, and permit sleep with
established cycling-sensor links. The new explicit `t5s3-painter-battery-opt`
profile was first flashed at 21:08 local on September 10, then updated at
21:13 with a sensor timestamp-race fix. It retains fixed 240 MHz
and MAIN_XTAL. The previous `t5s3-painter-gps-wake` profile remains the measured
fallback; neither the shipping default nor phone app was changed.

**Unplugged result:** GPS reception and SD checks passed through actual sleep.
The firmware recorded 38,578 successful sleep calls. Both HR and power links
sent regular notifications through about 13 minutes of sleep, but the power
link timed out at 22:03:14, three seconds before USB returned. The fallback
correctly disarmed established-sensor sleep for this boot. Whether the sensor
was stopped/moved or sleep caused the timeout is not established; the user has
been asked. HR and phone remained connected. See the result section below.

## Implementation

- L76K-only output configuration: GGA/RMC and GSA at every 1 Hz fix; GSV once
  every five fixes. Disable GLL/VTG/ZDA/ANT and reserved outputs. Positioning
  frequency, all three enabled constellations, baud and GPS power are preserved.
  RMC supplies speed/course/date/time; GGA supplies altitude/satellite count/HDOP.
  GSA maintains fix-quality telemetry. Satellite detail updates less often.
  Commands use computed XOR checksums and are reissued after cold start and
  module reinitialization. Configuration requests are logged; observed raw
  sentence counts establish whether the receiver actually applied them.
- HardwareSerial receive callbacks notify the GPS task from the UART event
  task. Counting notifications survive arrival before the task blocks. A
  one-second timeout keeps command/housekeeping latency bounded if GPS is silent.
  While the guard is active it revisits the quiet deadline every 10 ms.
  FIFO threshold 64 avoids Arduino's default one-byte interrupts at 9600 baud;
  two-symbol RX timeout drains the tail of a burst. The retained GPIO wake,
  FIFO/frame checks and raw checksum/epoch-loss fallback remain intact.
- Sensor links may release CPU and modem-sleep holds only when the MAIN_XTAL
  adapter is ready and the per-boot sensor experiment is armed. Hunt/connect
  holds remain. One supervision timeout, or more than 15 seconds without
  notifications on a connected link, disarms the experiment if successful CPU
  sleep occurred since connection/re-arm. This is a conservative correlation,
  not proof of causation: an inactive/out-of-range sensor can also trigger it.
  The PM task restores its usual holds. `sensorsleep [on|off]` controls this boot;
  without an argument it prints status. Other profiles cannot arm it.
- Sensor telemetry records connection/drop counts, notification counts/age and
  maximum gap, actual interval, and sleep calls since connection. `srlx` on PM
  lines is informational, not a hold. Existing bounded diagnostics preserve
  these records in RAM if SD cannot mount. No SD operation occurs in UART
  callbacks; they only notify the GPS task.

## Verification

Host ASan/UBSan suite passed, including the production 10/50 ms guard variants,
entry races, FIFO/frame protection, wake cause, quiet deadlines, setup failures,
stock no-op path, sensor eligibility/crystal/hunt/fallback PM holds, notification
silence and timeout correlation, timer wrap, a notification arriving after the task samples its clock, and
one-shot fallback. Existing
logger/SD/BLE/display regressions also passed.

Candidate and stock `t5s3-painter` firmware builds passed. The stock build first
caught an accidental duplicate guard implementation in its excluded-code path;
that was corrected and covered by a new no-op test. The candidate's compiled
path was unaffected. No shared SDK/package files were edited. The new private
libdeps directory was copied from the existing GPS-wake environment so the
same dependencies could build without downloading replacements.

Local build evidence: `.pio/battery-opt-host-tests-accepted.log`,
`.pio/battery-opt-build-final.log`, `.pio/battery-opt-stock-build-final.log`.
Flashed binary fingerprint: `pm-battery-opt-candidate` in
`battery-sd-firmware-images.json`.

Initial hardware window at 21:08:59: 15 GGA, 15 RMC, 45 GSA, 15 GSV and zero
other sentences over 15.009 seconds; 3,984 bytes, 90 valid sentences, no bad
checksum/truncation/missing epochs. UART FIFO/frame/parity counters were zero;
buffer counter was one from startup and did not increase in that window.
Subsequent awake windows at 21:10:00, 21:10:15 and 21:10:30 remained clean
(3,984, 3,984 and 4,048 bytes). Satellite C/N0 reporting recovered to 29;
raw output confirms satellite details still arrive every five fixes.
The same phone link (ID 1) negotiated 300 ms. Three SD write/close/reopen/compare/
remove checks passed on USB. These are not sleep-test passes.

Final build at 21:13 also passed awake reception: full 15-second windows retained
15 GGA/15 RMC with no corrupt or missing epochs, and all UART error counters
were zero. Four SD checks passed in the final capture; the test is still running.
Phone ID 1 reached 300 ms and subsequently requested the normal fast interval
for bulk traffic. No sensor links were up. The 30-minute test was armed at
21:13:12 and expires near 21:43:12; auto-shutdown OFF was confirmed at 21:13:11.
Raw serial evidence: awake captures (local-only evidence).

## Battery-test procedure and remaining work

Auto-shutdown is disabled for this boot. A 30-minute `sdtest` is armed before
handoff; its timer starts immediately, not at USB removal. Use only individual
checks marked with positive `preceding_sleep_calls` as checks after actual sleep.
Phone may stay connected. Keep any test sensors transmitting and establish their
links before unplugging. The Sensors screen forces continuous scanning, so
return to the dashboard before measuring established-link sleep. If sensors
are unavailable, GPS and SD can be validated separately; report sensor acceptance
as pending rather than treating absent links as a pass.

Unplug for at least five minutes, then reconnect and retrieve `sdtest status`,
`pm`, `sensorsleep`, `bleinterval status`, and `diag sd 32768`. Decode the SD tail
with `tools/power_sd_decode.py`; require a complete stream. Check raw 1 Hz GPS
epochs, no new UART errors, sensor notification continuity/drop counts, the same
phone link, SD integrity and actual battery samples. Log timestamps may shift
when GPS updates wall time. Long navigation, another GNSS module, deep sleep,
and sensor reconnection on a real ride remain separate acceptance tests.

Protocol reference: [Quectel L76K protocol, PCAS03](https://files.waveshare.com/upload/d/dd/Quectel_L76K_GNSS_Protocol_Specification_V1.1.pdf).
HardwareSerial event behavior was checked against the local Arduino 2.0.14
`HardwareSerial.cpp/.h`, including its one-byte low-baud default.

## Unplugged result retrieved at 22:04

- PM totals: 38,578 successful calls, 20 rejected calls, 1,093.486 seconds
  inside successful calls including entry/exit overhead. These are cumulative
  boot counters; do not divide by total boot uptime and call it sleep residency.
- Recovered SD range covers approximately 21:31–22:04. In 129 GPS windows
  containing sleep: 1,935 GGA and 1,935 RMC, 11,744 valid sentences, 515,025
  bytes; zero bad checksums, truncated sentences, missing GGA/RMC epochs, or
  new UART FIFO/buffer/frame/parity errors. The receiver was searching, so
  navigation/fix accuracy remains untested.
- SD test completed at 21:43:12: 352 passes, zero failures/skips; 206 checks
  followed actual sleep and retained the same phone link. Its 14,713 sleep
  calls are the counter delta during the test, not the entire run. The test
  ended before cycling sensors connected, so those 206 checks cannot be
  claimed as explicit integrity tests with sensors connected. Persisted logs
  demonstrate SD writes during the later sensor-connected phase.
- HR connected at 21:49:52 and power at 21:49:55. Both were at 50 ms BLE
  intervals, sending roughly 1 Hz notifications through 17,234 additional
  sleep calls. At retrieval: HR 880 notifications, maximum observed gap 1,200
  ms, no disconnect; power 798 notifications, maximum observed gap 1,100 ms,
  then one supervision timeout at 22:03:14. The last power notification was
  near 22:03:11. USB returned at 22:03:17. Sensor addresses were the paired
  c6:5e:11:00:00:01 and c6:5e:11:00:00:02; advertised identities alone do not
  establish whether these are physical production sensors or test emulators.
- Phone connection ID 1 persisted. It negotiated the expected idle 300 ms and
  temporary 30 ms during bulk traffic. No phone disconnect appears in the
  recovered range.
- In 16 complete phone-only PM windows (21:33–21:48), time inside sleep calls
  was 49.85% of elapsed time. In 12 complete established-sensor windows
  (21:51–22:02) it was 44.09%. This includes sleep entry/exit overhead; neither
  percentage is pure hardware sleep residency. Previous GPS-wake windows
  spent approximately 13% inside these calls.

### Current and runtime estimates

Phone-only discharge samples: 85, 88, 83, 114, 79, 109, 75 mA; mean **90.43 mA**.
All seven samples are included, including the two higher readings. This implies
**16.6 hours** at the board-reported 1,500 mAh capacity, compared with **15.5
hours** from the prior two 97 mA samples. These are separate runs, not a
controlled A/B or full-discharge test.

Established HR + power + phone samples: 101, 98, 103, 101, 101 mA; mean
**100.8 mA**, implying **14.9 hours** if that load persists. The earlier
**179 mA discovery/connection sample** is reported separately because PM
explicitly recorded the hunt hold. Including it makes the six sensor-phase
sample mean 113.83 mA; it must not be silently omitted from a whole-phase claim.

### Evidence and board state

Decoded result and live status (local-only evidence) and
machine-readable totals (local-only evidence). A larger serial dump hit
its bounded 30-second deadline; the decoder correctly rejected it as incomplete.
Its contiguous prefix and the prior complete 32 KiB tail were joined only after
verifying 20,541 identical overlap bytes. Reconstructed range: [991928,1120123),
128,195 bytes. No missing bytes were inferred.

The board remains on the final candidate. CPU light sleep is requested ON,
but **sensor sleep is disarmed after the power timeout**, so the still-connected
HR link holds sleep off; USB also holds it off while plugged in. No automatic
re-arm or flash was performed during retrieval. Auto-shutdown remains OFF for
this boot; the SD test is complete. The power timeout's cause is pending user
context before accepting sensor sleep without qualification.

Raw logs, crash dumps, and exported investigation results are retained locally in ignored paths. They must not be committed: diagnostic breadcrumbs and captured memory can contain GPS coordinates.
