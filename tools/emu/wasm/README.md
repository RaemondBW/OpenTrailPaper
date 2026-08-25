# Phase 3: QEMU in the browser (WebAssembly)

Goal: drop the Python bridge and the native QEMU process — run the emulator
itself in the page, so the site is fully self-contained.

## Approach

Compile the Espressif QEMU fork (esp32s3 machine) to WebAssembly with
Emscripten. Prior art: ktock's `qemu-wasm` patches carry mainline QEMU to
`wasm32-emscripten`; the same treatment applied to the Espressif fork is the
task. The page from phase 2 is reused unchanged — its frame decoder and event
encoder are transport-agnostic; only the byte pipe changes from a WebSocket to
an Emscripten `MEMFS`/`postMessage` channel between the WASM worker and the UI.

## Build sketch

```sh
# 1. Emscripten SDK
git clone https://github.com/emscripten-core/emsdk && cd emsdk
./emsdk install latest && ./emsdk activate latest && . ./emsdk_env.sh

# 2. Espressif QEMU, configured for wasm (xtensa-softmmu only, TCG interpreter)
git clone https://github.com/espressif/qemu && cd qemu
emconfigure ./configure --target-list=xtensa-softmmu \
    --cross-prefix= --cc=emcc --cxx=em++ --host-cc=cc \
    --enable-tcg-interpreter --disable-tools --disable-plugins \
    --static --without-default-features
emmake make -j
# -> build/qemu-system-xtensa.wasm + .js loader
```

## Open items (blocking, in order)

1. **PSRAM** — the same esp32s3 model gap as native (see
   investigations/web-emulator-status.md). Must be fixed first: it is what
   currently keeps the UI loop from running, WASM or not.
2. **Threads** — QEMU's TCG uses host threads; under wasm this needs
   `-pthread` + SharedArrayBuffer (COOP/COEP headers) or the single-threaded
   TCI path above. TCI is slower but avoids the threading/headers problem.
3. **Block + chardev** — the merged flash image ships as a fetched asset into
   MEMFS; the two UARTs become in-memory ring buffers the JS reads/writes
   instead of TCP sockets.

The transport seam is already in place, so phase 3 is a QEMU-build project, not
a rewrite of the emulator.
