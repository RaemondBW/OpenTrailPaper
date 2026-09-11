# Crash reports on the battery candidate

On a panic, interrupt/task/other watchdog reset, or brownout, the next boot now
creates a short report even when no core dump was saved. Full crash capture now
uses [Memfault with offline SD export](memfault-offline.md). The short report contains:

- reset cause, boot firmware/build and ELF SHA256;
- boot UTC value, explicitly marked as potentially preceding clock restoration;
- Memfault pending-dump status, byte count, and ID;
- the last ten diagnostic lines retained in RTC memory, with prior-boot uptime.

The report is staged in the `crashdiag` NVS namespace before attempting SD
mount. Up to four reports are queued, subject to available NVS space. Existing
queued reports are not overwritten when full: the new one remains in RAM and
is delivered directly if SD becomes available. If neither durable store works,
that RAM-only report can still be lost at the next reset/power loss; this is
logged explicitly. RTC breadcrumbs are bounded and do not survive removal of
RTC power. A reset during a breadcrumb write invalidates that entry via CRC.
This is normal boot-time logging, not SD access from a panic/interrupt handler.

After mount, the main task writes `/logs/crash-XXXXXXXX.tmp`, closes it, reads
back and compares every byte, then renames it to `/logs/crash-XXXXXXXX.log`.
Only after the final file verifies does it remove the NVS copy. Failed/short
writes retry after 30 seconds. A remount also permits retry. USB mass-storage
ownership prevents SD access. An interrupted acknowledgement detects the same
completed file and removes the NVS entry without appending duplicate content.
A conflicting completed filename is not overwritten.

The Memfault dump is acknowledged only after its full chunk export verifies
on SD. Saving this short report does not acknowledge it. In the earlier legacy
implementation, ESP-IDF summary durability was the acknowledgement boundary;
that path remains covered by host tests but is disabled in Memfault builds.
When no dump exists, reset cause and breadcrumbs cannot establish the faulting
function. Keep the exact build ELF for decoding.

The existing phone Diagnostics log list includes `crash-*.log`; download it
like a daily log. Serial commands:

```text
crashlog          print pending/current-boot report and retry SD delivery
crashlog status   same
crashlog panic    DELIBERATELY crash/reboot to test the feature
```

The test panic is refused while a ride is recording. It marks the RTC log with
`CRASH TEST` so it can be distinguished from an unexplained field crash. A
normal software restart or deep-sleep wake does not create a new crash report,
but still delivers any previously queued reports. Completed reports remain on
SD; the feature does not automatically delete them.

Diagnostic SD flushing during recording now has a 30-second maximum batching
age (subject to task/SD availability), in addition to the size threshold. This
reduces the several-minute gaps in the Sep 11 ride evidence. Failed SD writes
still retain buffered data and use the existing backoff; this does not promise
lossless logs across arbitrary crashes or long card outages.

Host validation:

```sh
python3 tools/crash_test/run_tests.py
python3 tools/power_test/run_tests.py
PLATFORMIO_CORE_DIR="$PWD/.pio/core" pio run -e t5s3-painter-battery-opt
```

The crash suite compiles the production service with mocked hardware under
ASan/UBSan (fatal on sanitizer errors). It covers crash/normal boot, RTC and
report CRC, missing NVS/SD, short write, failed rename, failed NVS removal,
reboot before delivery, idempotent retry, four-slot overflow, USB ownership,
core acknowledgement only after durability, and the active-ride panic guard.
The logger suite tests sparse ride flush deadlines and timer wraparound.

## UART crash found during validation

Before the deliberate test was reached, the device produced a real panic in
`uart_event_task`. Report `3550844e` preserved its trace through
`HardwareSerial::_uartEventTask` → `__wrap_log_printf` → `log_printfv` →
`vsnprintf` → interrupt context save. The UART buffer-full warning was being
formatted on a 2048-byte task stack. This identifies the observed path; it does
not establish the cause of the earlier interrupt-watchdog road resets.

UART driver messages now use the same deferred queue as SD driver messages,
avoiding the additional formatting/allocation/UART0-locking route on that task.
The battery candidate also allocates 4096 bytes to its UART event task. The
host regression checks that UART errors are queued and not forwarded; the
original crash ELF is preserved as `.pio/crash-report-3550844e.elf`.

## Earlier legacy hardware validation — Sep 11

Before the Memfault integration, the candidate was flashed to the connected LilyGo. A normal boot mounted
SD on the first attempt and created no new report. The phone connected and
negotiated a 300 ms idle interval. A subsequent guarded `crashlog panic` test
rebooted with report `93f168e3`: NVS staging succeeded before SD mount, then
`/logs/crash-93f168e3.log` was written and read-back verified (1558 bytes), and
the NVS queue returned to zero. Its RTC tail contains the deliberate test
marker, and its backtrace decodes to `crash_report::tick` → `abort` on `loopTask`.
The exact ELF is `.pio/crash-report-final-tested.elf`.

The UART buffer-full warning occurred during that reboot and was deferred;
startup and crash-report delivery completed without another panic. A deferred
VFS “does not exist” message reflects the initial completed-file probe before
creation, not failure of the subsequently verified write.

Both battery and stock firmware builds and the two host suites passed.
Physical testing used mounted SD and USB power (CPU sleep held off); storage
outage/fault paths were exercised under mocks. This is not a new road stability
or battery test. Reports and checks for that earlier validation are preserved in
`crash-feature-validation.json` (local-only evidence).

Raw logs, crash dumps, and exported investigation results are retained locally in ignored paths. They must not be committed: diagnostic breadcrumbs and captured memory can contain GPS coordinates.
