# Web emulator — the real firmware binary, booted in a browser

Goal: a web page that runs the ACTUAL firmware — not a screenshot generator,
not a re-implementation — with the e-paper panel drawn on a canvas and the
buttons/touch usable. tools/preview already fakes the *renderers* on the host;
this is the opposite ambition: emulate the *machine* and let the shipped code
run.

## Architecture

Three layers, each independently useful:

```
[t5s3-emu firmware build]  --UART1 frame/event protocol-->  [panel UI]
        runs inside                                          HTML canvas
[Espressif QEMU, esp32s3 machine]
        which runs either
[natively + WebSocket bridge]   (phase 2 — a served emulator)
[compiled to WebAssembly]       (phase 3 — fully in-page)
```

**The firmware is the real one, built for the machine it runs on.** The
device firmware already has swappable panel backends behind `epd_compat.h`
(epdiy vs EPD_Painter — 10 functions); `epd_compat_emu.cpp` is a third:
instead of driving LCD_CAM it streams the 4bpp framebuffer, RLE-compressed,
out UART1, and accepts button/touch events back on the same wire. Same
renderers, same tasks, same recorder, same diag — a build variant exactly the
way `t5s3-pro` vs `t5s3-painter` are variants, not a fork.

QEMU's esp32s3 machine emulates CPU/ROM/flash/UART/timers well and our
missing peripherals mostly hit paths the firmware already survives in the
field: no SD card (tolerated, logged), no fuel gauge (logged, blank battery),
no touch controller (buttons still work), LoRa init fails (logged, mesh off).
The two that do NOT degrade are compiled out under `-DOTP_EMULATOR`:

- **NimBLE** — there is no BT controller model in QEMU; `esp_bt_controller_init`
  does not fail politely. Both stacks and their tasks are guarded out.
- **Light sleep / PM** — meaningless under emulation; `PM_LIGHT_SLEEP=0`.

## Wire protocol (UART1, both directions)

Frames out:  `0xF5 'F' [u16 seq] [u32 rleLen] <RLE bytes> 0xF6`
  RLE: `[count u8][value u8]` pairs over the 4bpp buffer (540x960/2 bytes).
  E-paper content is mostly runs of white; a dashboard frame compresses ~30x.
Events in:   `0xE1 [key u8]` press (1=BOOT 2=side 3=home)
             `0xE2 [u16 x][u16 y]` tap
Fed to the firmware through the same debounced paths real inputs take.

## Phases

1. **Emu backend + t5s3-emu env builds; boots in native QEMU.** Espressif
   QEMU fork (esp32s3 machine), merged flash image, boot log on serial0,
   frames appearing on serial1. This is the risk-retirement step.
2. **Web page + bridge.** A ~100-line Python websocket bridge pipes serial1;
   `web/emulator.html` draws the panel (canvas, e-paper styling), sends
   events. "A website running the device" — served, not yet serverless.
3. **QEMU-in-WASM.** Compile the Espressif QEMU fork with Emscripten (prior
   art: ktock's qemu-wasm patches for mainline). The page from phase 2 keeps
   its protocol; only the transport changes (websocket -> wasm pipe). This is
   the hard, long tail — phase 2 stands on its own if it stalls.

## Risk register

- QEMU esp32s3 + OPI PSRAM: the machine models octal PSRAM size but timing
  differs; the epdiy-format framebuffer lives in PSRAM. If allocation works,
  we're fine — no LCD_CAM involvement in the emu backend.
- UART1 in QEMU maps to `-serial` #2; throughput is host-speed, not baud, so
  1 Hz frames are trivial.
- The GPS task reads UART2; QEMU exposes it as serial #3 — NMEA can be piped
  in for a simulated ride (RideSim's routes exist in the app repo for data).
- qemu-wasm (phase 3) has never been done against the Espressif fork as far
  as searching shows. Budget it as its own project.
