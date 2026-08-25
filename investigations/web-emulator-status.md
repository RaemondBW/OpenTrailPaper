# Web emulator — status (phases 1–3)

## Working, end to end

**The real firmware binary boots in QEMU and renders its actual UI into a
browser, inside the site's device model, with the buttons and glass wired up.**
- `pio run -e t5s3-emu` builds the shipping firmware with the emulator panel
  backend (`epd_compat_emu.cpp`) — same renderers, recorder, tasks, diag.
- `tools/emu/run-qemu.sh` boots it in Espressif QEMU (esp32s3), streaming the
  4bpp framebuffer out UART1 as RLE frames.
- `tools/emu/serve.py` bridges QEMU to a WebSocket; `web/emulator/` draws the
  panel on a canvas set into the site's `.device-mock` (white LilyGO body,
  round home key, edge buttons) and turns the buttons/home/glass into input
  events. Sensor and battery spoof sliders and a ride simulator are in the page.
- Verified in a real browser: the firmware's own dashboard, boot screen, and
  power sheet render pixel-for-pixel; the frame stream decodes cleanly
  (`tools/emu/frame2png.py`). Earlier browser captures showed the live
  dashboard with spoofed HR/power values on screen.

## Two QEMU-model limits, and where they leave input

The esp32s3 machine in the current Espressif QEMU release has two gaps that
input runs into. Both are emulator-integration issues, not firmware bugs.

1. **No UART RX.** The model delivers UART TX perfectly (frames flow) but never
   raises RX on any port (UART0/1/2), on both the 2025 and 2026 releases, with
   and without FIFO-filling padding. So input cannot arrive over a serial line;
   `serve.py` instead pokes events straight into a DRAM mailbox
   (`g_emuMailbox`, `emu_input.h`) over QEMU's gdbstub. Memory writes through
   the stub are confirmed landing (magic + readback verified).

2. **No PSRAM.** PSRAM init fails (`PSRAM ID read error … wrong PSRAM line
   mode`) under both the octal (`qio_opi`) and quad (`qio_qspi`) framework
   variants, so `heap_caps_malloc(MALLOC_CAP_SPIRAM)` falls back to internal
   SRAM for the 259 KB framebuffer, and the mesh ring / map cache (SPIRAM-only)
   are disabled. The device tolerates all of that — but the current build's UI
   task loop stops running after "all tasks started" (no panic, no crash, the
   loop is simply never entered on the emulator). Earlier builds DID render the
   live dashboard, so this is a regression that arrived with the input plumbing;
   the exact interaction was not isolated before I stopped to consolidate.
   Because the loop doesn't run, `pump()` never drains the mailbox, so on-screen
   input does not yet round-trip.

**Next steps, in order:**
1. Bisect the UI-loop stall against the last known-good display build (the
   Serial1-pump build that rendered the dashboard) — candidates are the GPS
   ring shim (`EmuGpsSerial`), the 4 KB mailbox `.bss`, and no-PSRAM heap
   pressure. This is a mechanical bisect, not open-ended.
2. Give QEMU real PSRAM (machine memory config / a QEMU build with the esp32s3
   PSRAM model) so the framebuffer leaves internal RAM — the hardware layout.
   The firmware and page halves of input are complete and protocol-validated;
   the mailbox write path is confirmed working. Only the guest loop running
   again stands between here and interactive input.

## Phase 3 — QEMU in WebAssembly

Scaffolded in `tools/emu/wasm/` (build recipe + notes). The page is already
transport-agnostic: the frame decoder and event encoder don't know whether the
bytes come from a WebSocket (phase 2) or an in-page WASM pipe (phase 3) — only
`serve.py`'s role changes. This is the "hard long tail, its own project" the
plan flagged; it depends on the same PSRAM fix, since a WASM QEMU inherits the
same machine model.
