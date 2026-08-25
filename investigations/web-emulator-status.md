# Web emulator — status

## Working, verified in a browser

**The real firmware binary boots in QEMU and is INTERACTIVE in a web page,
inside the site's device model.**

- `tools/emu/run-qemu.sh` boots the `t5s3-emu` build (the shipping firmware
  with an emulator panel backend) in Espressif QEMU; `tools/emu/serve.py`
  bridges it to the browser; `web/emulator/` draws the panel on a canvas set
  into the site's `.device-mock` (white LilyGO body, round home key, edge
  buttons).
- The device **finishes booting to its dashboard** and renders pixel-for-pixel
  — the shipping renderers, not screenshots.
- **Input round-trips, confirmed on screen:** the HR / power / cadence / battery
  sliders live-drive the real dashboard (power with its FTP zone bar, HR, cadence,
  the status-bar battery). Buttons, the home key and the touch glass send the
  same event protocol; the GPS ride simulator feeds NMEA.

## Peripherals — how each is emulated (all in `epd_compat_emu.cpp` / `emu_input.h`)

QEMU's esp32s3 machine models the CPU/RAM/flash/UART/timers but none of the
board's peripherals, so each is emulated in the firmware under `OTP_EMULATOR`
and driven from the browser over a byte protocol:

| Peripheral | How |
|---|---|
| E-paper panel | framebuffer streamed out UART1 as RLE frames -> canvas |
| Buttons (BOOT, side) | web events -> DRAM mailbox -> `emu_input`, same debounce |
| Touch (GT911) | pointer events -> mailbox -> the UI's tap dispatch |
| Home key | one-shot event -> the GT911 home callback path |
| HR / power / cadence | slider events -> `RideState` (NimBLE is compiled out) |
| Battery (fuel gauge) | slider event -> `RideState` battery/charging |
| GPS (CASIC/UART2) | NMEA events -> an in-guest ring standing in for `SerialGPS` |
| SD card | flash "storage" FAT partition mounted at `/sd` (see below) |
| LoRa / RTC / IO expander | absent, tolerated exactly as on a cardless real device |

**Input transport:** the esp32s3 model delivers no UART RX, so events are
written into a DRAM mailbox (`g_emuMailbox`) through QEMU's gdbstub — the bridge
connects, writes, and detaches per flush so the guest runs free between events.
Proven working (the sliders drive the panel through it).

## The one remaining blocker: PSRAM

QEMU's esp32s3 exposes **no PSRAM** (the firmware's probe reads chip id 0). On
hardware the 259 KB framebuffer, the SD/FAT buffers and the map/mesh caches all
live in PSRAM; with it absent they fall back to internal SRAM (512 KB total),
which the framebuffer alone nearly fills. Consequences and how they're handled:

- **Boot / render / input: solved.** The UI task's stack and the input mailbox
  were right-sized so the device boots and runs in the internal-RAM-only budget.
- **SD card: implemented, but cannot mount yet.** `mountLocked()` under
  `OTP_EMULATOR` mounts the flash `storage` FAT partition at `/sd` and points the
  Arduino `SD` object at it (persistent across reboots) — the correct emulation,
  and every `SD.*` call would hit it. It currently returns `ESP_ERR_NO_MEM`
  because FAT's buffers don't fit in the internal RAM the framebuffer occupies.
  A cardless device is a supported state, so this degrades gracefully (no
  recording; the device runs). Ride recording, map/route/config persistence wait
  on this.

**The single fix that unblocks the rest is real PSRAM in QEMU.** With the
framebuffer back in PSRAM (its hardware home), internal RAM frees up and the SD
FAT mounts. QEMU has the `ssi_psram` model and an `esp32s3.cache.psram_as`
address space, but the esp32s3 machine gates PSRAM on the efuse configuration,
which needs an espressif-generated efuse blob (`espefuse` / `idf.py qemu`) — the
one piece of espressif tooling not available in this environment. Once that
efuse is provided (or QEMU is built with PSRAM defaulted on), no firmware or
page change is needed: the SD emulation and everything downstream come alive.

## Phase 3 — QEMU in WebAssembly

Scaffolded in `tools/emu/wasm/` (Emscripten build recipe + blocking items). The
page is transport-agnostic, so phase 3 is a QEMU-build project, not a rewrite —
and it inherits the same PSRAM prerequisite.
