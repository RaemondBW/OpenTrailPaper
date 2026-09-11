# Offline Memfault crash capture

The firmware now uses the pinned Memfault Firmware SDK for panic capture,
reboot tracking, RAM log collection, packetization, and build IDs. Its official
ESP-IDF 4.x panic and flash-storage ports compile against the existing Arduino
2.0.14 framework, including the private battery SDK. No framework migration,
new network connection, upload task, or heartbeat timer is introduced.

Source provenance and license: [vendored SDK](../third_party/memfault-firmware-sdk/UPSTREAM.md).
Integration follows the [Memfault ESP32 guide](https://docs.memfault.com/docs/mcu/espressif-esp32-guide),
with a local offline transport in place of HTTP. The offline files are prepared
for later ingestion by Memfault's tools; no project key is embedded and no data
is uploaded automatically.

## Crash and recovery

1. ESP-IDF's panic path invokes Memfault's handler. It stores a Memfault-format
   dump in the existing 64 KiB internal-flash coredump partition. No SD, NVS,
   heap allocation, or application log mutex is used by our panic-region hook.
2. Next boot initializes Memfault and records the hardware/software reset
   reason. The SDK reset-event collector consumes the prior RTC tracking record
   so the next panic can record its own reason. This build exports coredumps;
   standalone reboot-event/metrics uploads are not enabled.
3. The main task exports the pending dump using the SDK's packetizer and
   `memfault_data_export_chunk`. `/logs/memfault-<id>.log` contains standard
   `MC:<base64>:` lines, recognized by `memfault-cli`. The ID is the first
   128 bits of the dump's SHA256.
4. It writes a temporary file, closes it, regenerates and compares every export
   byte, renames the file, then verifies the final file. Only then does it
   acknowledge the internal-flash dump. The SDK's ordinary final-chunk
   acknowledgement is intercepted and deferred until this point.

If SD fails to mount, is owned by USB mass storage, or has an I/O failure, the
first pending dump remains in internal flash across reboot and power removal.
Retries occur every 30 seconds. A second panic while a dump is pending preserves
that first dump; this is a one-dump queue, not unlimited crash storage. Existing
matching exports are acknowledged without duplication; conflicting files are
not overwritten. A flash-read error aborts export even though the SDK's
packetizer normally substitutes an error marker and continues.

`crash-*.log` still provides a small human-readable reset report and RTC log tail,
with the pending Memfault ID. These short reports use their existing NVS queue
if SD is absent. Saving a short report never acknowledges the Memfault payload.
A brownout/hard reset that bypasses the panic handler can provide a reset report
without registers or stack data. See [short crash reports](crash-reports.md).

## Captured information and limits

- Both CPU register frames, when ESP-IDF supplies them.
- Up to 8 KiB of each stopped CPU's internal-RAM stack, including Xtensa spill
  space, plus the current task pointers and up to 512 bytes of each current TCB.
- A 2 KiB Memfault diagnostic-log ring and the first 32 KiB of internal `.bss`.
- Device serial from the chip MAC, software/hardware identifiers, trace reason,
  and the Memfault build ID embedded in the exact ELF.

This bounds collection within the existing partition without traversing task
lists in panic context. The precompiled FreeRTOS library has no Memfault task
registry hooks: all sleeping-task stacks, all heap memory, and PSRAM are **not**
collected. A missing other-core frame is represented by empty registers. Keep
these limits in mind when investigating deadlocks involving other tasks.

Memfault log entries are capped by the SDK's per-line limit; the existing daily
SD log and ten-entry RTC tail remain useful supplementary evidence. SDK logging
avoids UART0/GPS TX and skips logging from interrupt/critical context.

## Export without an account

Serial commands:

```text
memfault          status and retry pending SD delivery
memfault export   export pending flash data, or the file saved during this boot
crashlog          short reset report and NVS status
crashlog panic    deliberate test panic, refused during an active ride
```

Serial export itself does not acknowledge flash. A successful independent SD
archive may subsequently acknowledge it. If the connection breaks, rerun the
export from the beginning. After a later normal reboot, older files can be
downloaded through the phone's existing Diagnostics log list.

Capture with `tools/power_capture.py --command 'memfault export'`, or download
`memfault-*.log` from the phone. Then prepare a bundle locally:

```sh
python3 tools/memfault_export.py downloaded-memfault.log .pio/private-diagnostics/exported-crash
```

The new output directory contains `chunks.log`, `coredump.mflt`, and
`manifest.json`. The helper rejects incomplete/out-of-order chunks and validates
the SDK message CRC, coredump framing, and length. It reports build ID, register
PCs/SPs, memory regions, and whether memory capture was truncated. It does not
replace Memfault's full symbolication or perform an upload.

## Use Memfault cloud tools later

Once a project is configured, upload the **matching** ELF using the Memfault
CLI or dashboard, then submit `chunks.log` with `post-chunk --encoding
sdk_data_export`. The original SD log is already in that format. Commands,
using project credentials supplied locally:

```sh
memfault --org-token "$MEMFAULT_ORG_TOKEN" --org "$MEMFAULT_ORG" \
  --project "$MEMFAULT_PROJECT" upload-mcu-symbols matching-firmware.elf
memfault --project-key "$MEMFAULT_PROJECT_KEY" post-chunk \
  --device-serial "$MEMFAULT_DEVICE_SERIAL" \
  --encoding sdk_data_export .pio/private-diagnostics/exported-crash/chunks.log
```

These are manual future steps, not part of firmware boot or the export helper.
Set `MEMFAULT_DEVICE_SERIAL` to the bundle manifest's `device_serial` value.
See [Memfault symbol upload](https://docs.memfault.com/docs/ci/cli/upload-mcu-symbols)
and [chunk upload](https://docs.memfault.com/docs/ci/cli/post-chunk), plus the SDK's
[data-export API](../third_party/memfault-firmware-sdk/components/include/memfault/core/data_export.h).

`tools/memfault_build.py` runs the official `fw_build_id.py` after linking and
before creating the flash image. It archives each ELF under
`.pio/memfault-symbols/<build-id>/<elf-sha256>.elf`, preserving symbols across
rebuilds. Both identifiers are retained because different debug sections may
share the same Memfault build ID. Back up this local archive before deleting
`.pio` or the worktree. Older ESP-IDF core dumps are a different format and are
not converted.

## Validation

```sh
python3 tools/memfault_test/run_tests.py
python3 tools/crash_test/run_tests.py
python3 tools/power_test/run_tests.py
PLATFORMIO_CORE_DIR="$PWD/.pio/core" pio run -e t5s3-painter-battery-opt
```

The transport suite compiles the production transport with the actual pinned
SDK packetizer and exporter under ASan/UBSan. It injects missing SD, USB
ownership, short writes, bad readback, rename failure, flash read failure,
failed acknowledgement, reboot/retry, and conflicting filenames. It verifies
repeatable non-destructive serial export and successful SD delivery before
acknowledgement. The short-report suite covers both legacy and Memfault modes.
The same suite exercises the production phone download handler with a 45-byte
Memfault filename; its previous 40-byte truncation is corrected to match the
47-byte request capacity.

### On-board result — Sep 11

The Memfault capture candidate passed a normal boot and guarded deliberate panic.
After reboot, Memfault reported assertion reason `0x8001` and a 44,596-byte
dump. It wrote and read-back verified
`/logs/memfault-e5cdc8033ebd9bfcd91ba1b95b35c269.log` (66,005 bytes), then
acknowledged the flash copy. Retrieving that file from SD over serial produced
585 valid chunks with a matching CRC. The recovered dump contains the deliberate
test log, device `3c8427eea358`, version `v1.19`, and build ID
`365585e4d937e12ac17697bfc938cc2ab0a80bb4`, matching the archived ELF.
The saved PC decodes to `panic_abort`. No configured memory region was truncated;
ESP-IDF did not provide the other CPU's frame for this test.

After that capture test, the phone filename-length correction was built,
regression-tested and installed. Its normal boot confirmed SDK initialization,
SD mount and the empty acknowledged flash queue. That final image has build ID
`038f17c6517c9b5abd6d8458c053e374787630e0`; the crash capture implementation is
unchanged. Both matching ELFs are in the local symbol archive.

The host suites and battery/stock builds passed. Hardware testing used mounted
SD and USB power; missing/failed SD paths were tested under mocks. This does not
reproduce the earlier road watchdogs or validate battery endurance, and no
cloud ingestion/symbolication was performed. Idle automatic shutdown remains
disabled for this boot. The app resumes its normal setting on the next boot.

Validation record (local-only evidence)
and decoded export manifest (local-only evidence)
preserve the evidence.

Raw logs, crash dumps, and exported investigation results are retained locally in ignored paths. They must not be committed: diagnostic breadcrumbs and captured memory can contain GPS coordinates.
