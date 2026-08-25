# Web emulator — status

> **UI / input fixes (this session).**
> - **Both hardware buttons now sit on the LEFT case edge** — BOOT on top, the
>   backlight/side button below it (`web/emulator/style.css` `.side-*`).
> - **Long-press power dialog works.** The firmware logic was fine; the web
>   button released the press early on `pointerleave`. `wireButton` now uses
>   `setPointerCapture`, so a hold keeps registering and BOOT-hold (1.5 s) opens
>   the SHUT DOWN sheet.
> - **Map zoom + north-up buttons work.** They're drawn inside the map frame, so
>   the firmware handles the taps (touch already routes to it) and now reports
>   its zoom/orientation over a `0xF5 'M' <mpp> <trackUp> 0xF6` marker
>   (`epdc_emit_mapstate`, re-sent on the `0xE8` repaint). The browser renders
>   the WASM map at that state — verified 2 → 4 m/px (200 M → 500 M scale) and
>   track-up toggling.
> - **The device serial console is mirrored to the browser devtools console.**
>   QEMU's UART0 now goes to `tcp:5555`; `serve.py`'s `console_reader` forwards
>   it to every browser as channel 2 (and echoes it to the run log); the page
>   prints each line as `[device] …`. (`run-qemu.sh` serial0 moved off stdio.)


> **Ride recording + workouts work; maps need PSRAM.**
> - **Ride recording** runs under emulation without SD: `startRide()` skips the
>   `sdOk` gate and the `fit.begin()` open under `OTP_EMULATOR`, and the task
>   loop already keeps the timer/distance/metrics live while the FIT handle is
>   not open (all `fit.*` writes are `if(!file_)` no-ops). Tap BOOT to start —
>   RIDE TIME and DISTANCE advance from the spoofed GPS. The ride just isn't
>   persisted to a `.fit` file.
> - **Workouts** load without SD/BLE: `workout_service::loadText()` parses an
>   in-memory ERG string straight into `g_wk` (no PSRAM parse buffer), driven by
>   the web "Load a workout" button (mailbox `0xE9`, a sample sweet-spot
>   workout). The emulator layout is dashboard + map + workout (home cycles them).
> - **Maps render in the BROWSER, not QEMU.** QEMU can't draw the map — its
>   `ssi_psram` model never initialises (the 2nd-stage bootloader reads ID
>   `0x00000000`, "PSRAM chip not found", in BOTH quad and octal builds; enabling
>   it with `-m 4M` instead HANGS the guest, both cores spinning — upstream bug
>   github.com/espressif/qemu/issues/129), so the projector's ~700 KB of
>   `MALLOC_CAP_SPIRAM` scratch can't allocate. Solution: the firmware's OWN map
>   renderer (`map_tiles` + `ui_render_map` + the `epd_compat` rasteriser) is
>   compiled to WebAssembly with the SF map embedded — `web/emulator/map_wasm.cpp`,
>   built by `tools/emu/build-map-wasm.sh` (needs emsdk). The page draws it on the
>   map page from the same GPS/sensor state it feeds the device, producing a
>   pixel-identical map frame (same rasteriser, same `EPD_ROT_PORTRAIT`). The
>   firmware announces the active view over a `0xF5 'P' <code> 0xF6` marker
>   (`epdc_emit_view`, re-sent on the `0xE8` repaint); on the map view the page
>   ignores QEMU's "NO MAP HERE" frame and blits the WASM map instead.
> - The old blockers still stand for on-DEVICE map/SD (they need a QEMU whose
>   esp32s3 MSPI PSRAM works); the WASM route sidesteps them for the map DISPLAY.
>   For the record: with real PSRAM, maps + persisted rides come
>   alive with no further firmware change.


> **Stability fixes (this session).** Three bugs that made the running stack
> look broken are fixed:
> - `tools/emu/serve.py` had `MAILBOX_SIZE = 4096` while the firmware ring is
>   `512` (`EMU_MAILBOX_SIZE`). The bridge wrapped writes at 4096 and computed
>   free space against the wrong size, so input events landed **past** the
>   512-byte buffer, corrupting adjacent BSS — including `g_state`'s mutex,
>   which then tripped a FreeRTOS `xQueueSemaphoreTake` assert in
>   `SharedRideState::snapshot()` ~90 s in and crash-looped the guest. Sizes
>   now match; uptime is stable and frames deliver.
> - Removed a stale `mailbox.sock.close()` (left from the persistent-attach
>   design) that threw `AttributeError` and killed the bridge thread on every
>   ws disconnect.
> - The GPS ride sim now emits a `GPGSA` sentence, so the firmware sees a **3D
>   fix** (`type=3`) instead of `type=0`; the on-screen clock also sets from
>   GPS time. Ride recording still needs SD (below).
>
> **Boot time: ~37 s → ~8 s.** QEMU models no I2C/SPI slaves, so every hardware
> probe ran to its full timeout. Under `OTP_EMULATOR` the probes are skipped
> (their peripherals are all spoofed): fuel gauge (`BQ27220.init`), IO expander,
> PCF8563 RTC, GT911 touch, Qwiic sensor scan, and the GPS module-detection
> waits (`initL76K` + 2×2 s `waitForBytes`). See `main.cpp`, `ui_dashboard.cpp`
> (`beginPanel`), `gps_service.cpp`.
>
> **Input lag fixed.** The UI loop blocks on `xSemaphoreTake(uiWake, …)` released
> by input ISRs — which QEMU never fires, so web taps were only noticed on the
> idle-tick fallback. Under emulation: `UI_IDLE_TICK_MS` 200 → 16 (poll the
> mailbox ~60×/s), the button levels are force-read every loop (no GPIO IRQ to
> trigger the 200 ms-gated poll), and the emu button-release hold `MIN_HOLD_MS`
> 250 → 60. Touch and buttons now feel live.

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
