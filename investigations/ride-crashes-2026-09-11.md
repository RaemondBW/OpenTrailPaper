# Battery candidate: test-ride crashes, 2026-09-11

Examined `codex/battery-sd-sleep` at `19ad9ee` and the connected LilyGo
(3C8427EEA358). Running logs identify the Sep 10 21:12:10 build, matching the
retained battery candidate ELF. No firmware was built, flashed, or modified
for this investigation. The AGNSS worktree was not changed.

## Confirmed findings

Today's SD log contains two **interrupt watchdog** resets (`ESP_RST_INT_WDT`,
code 5), with restart log timestamps **09:07:22** and **09:34:36** local time.
These are reboot timestamps, not precise fault times. The first startup clock
also steps backward when RTC is restored, so do not infer elapsed time from
all early wall-clock messages without accounting for that adjustment.

| Restart | Last persisted pre-reset event | Recovery |
|---|---|---|
| 09:07:22 | 08:59:36: GPS fix, 18 satellites, clean 15-second sentence window; sleep active | SD mounted first try; SOC 96%; GPS first fix immediately at task startup; interrupted ride offered at 1.95 km / 332 s |
| 09:34:36 | 09:30:13: startup/recovery complete; interrupted ride offered at 1.95 km / 332 s | SD mounted first try; SOC 93%; GPS first fix immediately; ride recovery found 2.92 km / 543 s; continued at 09:34:56 |

Separately, **09:17:33 was normal idle auto-shutdown**, followed by a BOOT-button
deep-sleep wake logged at 09:30:02. It is not another watchdog crash. The rider
subsequently saved the resumed ride at **09:45:32: 5.79 km, 1101 active seconds,
80 paused seconds**. This confirms the logged recovered/saved totals, not that
every record from the original ride survived.

There are no brownout or failed SD mount records for either reboot. The last
persisted GPS window before the first reset has no checksum or missing-epoch
errors, so it does not show the earlier GPS/sleep corruption regression.
The 16:11:56 GPS-corruption checkpoint replayed at boot is explicitly labelled
an older retained failure snapshot; it must not be attributed to this ride.

## What cannot be recovered

No `CRASH dump` or backtrace appears in the complete recovered daily log.
The installed framework enables flash ELF core dumps and has a 300 ms
interrupt-watchdog timeout, but configured support does not establish that a
dump was successfully written.

After preserving serial/SD evidence, the board was briefly restarted into its
ROM loader. The actual partition table was read and its coredump partition
(`0xff0000`, 64 KiB) copied without writing firmware or flash data. The result
contains 65,524 `FF` bytes and 12 zero bytes, with no ELF signature or saved
stack payload. There is no crash PC/task/backtrace left to decode offline.
The bootloader's temporary force-download bit was cleared using the existing
flash helper, and the original application was restarted successfully.

An interrupt-watchdog reset identifies interrupt/tick starvation or a stalled
CPU/critical section; it does **not** identify the responsible function. The
new sleep/driver integration is worth investigating, but these records cannot
establish it as the cause, or isolate BLE, SD, GPS, display or a specific lock.
No root-cause claim or source fix is justified by the available trace.

The logger's current ride policy defers writes until half its 48 KiB staging
buffer is full. A reset can therefore lose the last several minutes of
breadcrumbs, consistent with these gaps. A future diagnostic build should
add a maximum flush age during recording, log unsuccessful core-dump retrieval
explicitly, and preserve minimal critical-section/sleep-entry breadcrumbs in
RTC memory. Compare the same ride with CPU sleep disabled versus enabled;
ordinary FreeRTOS/SD mutex waits alone are not proof of an interrupt watchdog.

## Evidence and device state

- Complete daily log (local-only evidence): 173,480 bytes.
- Retrieval manifest (local-only evidence): two
  offset-checked serial reads, with 44,760 bytes of identical overlap. The
  first read hit the existing 30-second limit; the second completed and fills
  the remainder. No gaps or differing overlap bytes exist in the reconstruction.
- Flash partition inspection (local-only evidence).
- Original raw serial captures and the flash binary remain under `.pio/ride-crashes-*`.

After the diagnostic restart, the SD was mounted and the existing PM policy
remained configured. `autosleep off` was set for this boot to prevent idle
shutdown during inspection. The competing QuntisBar serial utility was paused
only around serial access and resumed afterward. No sleep experiment or
recording was started, and no new code was committed or pushed.

Raw logs, crash dumps, and exported investigation results are retained locally in ignored paths. They must not be committed: diagnostic breadcrumbs and captured memory can contain GPS coordinates.
