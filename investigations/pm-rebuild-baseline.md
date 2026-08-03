# PM rebuild: baseline, blast radius, and how to get back

Read this **before** touching `CONFIG_PM_ENABLE`. Recorded 2026-07-31 against a
framework confirmed to build working firmware (v0.86, flashed and running).

The last attempt at this cost several hours and produced two lasting symptoms
that were not obviously connected to power management at all:

- **The SD card would not mount on *any* locally built firmware**, including
  commits from two weeks earlier that had worked fine.
- **Every package version string still matched CI**, so a version audit came
  back clean while the bytes on disk were not.

Those two together are the whole trap. The framework installs into an
**unversioned directory**, so a modified copy occupies the same slot and keeps
reporting the stock version. Nothing in PlatformIO will tell you.

    ~/.platformio/packages/framework-arduinoespressif32     <-- no version suffix

## 1. The current, known-good setup

| Component | Version |
|---|---|
| PlatformIO platform | `espressif32 @ 6.5.0` |
| Framework package | `framework-arduinoespressif32 @ 3.20014.231204` |
| arduino-esp32 core | 2.0.14 |
| ESP-IDF | 4.4.6 |
| Toolchain | `toolchain-xtensa-esp32s3 @ 8.4.0+2021r2-patch5` (GCC 8.4.0) |
| esptool | `tool-esptoolpy @ 1.40501.0` (esptool.py v4.5.1) |
| NimBLE-Arduino | 2.5.1 (pinned `^2.2.3`) |
| EPD_Painter | 2.1.0+sha.605089c |
| Build env | `t5s3-painter` (default); `t5s3-pro` is the epdiy fallback |
| Board memory type | `qio_opi` (QIO flash + octal PSRAM) — **load-bearing, see §3** |
| Git | `main` @ `d08d88d` |

**Content fingerprints** (SHA-256) — these, not the version strings, are what
tells you the framework is stock:

| Path (under `~/.platformio/packages/framework-arduinoespressif32/`) | SHA-256 |
|---|---|
| `tools/sdk/esp32s3/sdkconfig` | `b967289e…4507` |
| `tools/sdk/esp32s3/lib/libesp_pm.a` | `27fd6fe4…2c00` |
| `tools/platformio-build-esp32s3.py` | `4fd36c9a…827f` |
| `tools/sdk/esp32s3/` (whole tree, 2374 files) | `f69dc591…6a75` |
| `tools/sdk/esp32s3/qio_opi/` (live flash variant) | `bf4a9d91…3869` |

Verify any time with:

    tools/check-framework.sh          # full, ~30 s
    tools/check-framework.sh --fast   # skips the whole-tree hashes

It also does a semantic check that does not depend on any recorded hash: on a
stock framework `esp_pm_configure` in `libesp_pm.a` is an **8-byte stub** —

```asm
00000000 <esp_pm_configure>:
   0:   entry   a1, 32
   3:   movi    a2, 0x106     ; ESP_ERR_NOT_SUPPORTED
   6:   retw.n
```

— and on a PM-enabled rebuild it is a real function of several hundred bytes.
That single number is the fastest "which framework am I on?" test there is.

### Other package slots on this machine

There are three. Only the first is used by `t5s3-painter`; the other two are
left over from the ESP-IDF port experiment and are on a completely different
generation (arduino-esp32 3.3.11 / IDF 5.5). Do not confuse them.

    framework-arduinoespressif32                          3.20014.231204   <-- IN USE
    framework-arduinoespressif32@src-533e72d0…            3.3.11
    framework-arduinoespressif32-libs                     5.5.5+sha.b774170ff46

## 2. Why `CONFIG_PM_ENABLE` cannot be flipped in place

`framework = arduino` links **precompiled** archives. Espressif builds
`tools/sdk/esp32s3/**/*.a` once with a fixed sdkconfig and ships the binaries.
The `sdkconfig` file next to them is a *record* of that build — nothing in the
PlatformIO build reads it, and nothing recompiles from it.

```
tools/sdk/esp32s3/sdkconfig:1128:  # CONFIG_PM_ENABLE is not set
```

So `-DCONFIG_PM_ENABLE=1` in `build_flags` does nothing useful: it only affects
preprocessing of *our* sources. `esp_pm_configure()` and `esp_pm_lock_create()`
are already compiled as `return ESP_ERR_NOT_SUPPORTED` inside `libesp_pm.a`,
which is why `power_mgmt.cpp` logs `pm: light sleep UNAVAILABLE` and `tick()`
no-ops forever (null `s_usbLock`).

Watch out when grepping: the *sub*-options are `=y` under a parent that is off,
so the config reads as half-enabled when it is entirely off.

```
1128: # CONFIG_PM_ENABLE is not set
1129: CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=y
1130: CONFIG_PM_POWER_DOWN_TAGMEM_IN_LIGHT_SLEEP=y
2365: CONFIG_ESP_SYSTEM_PM_POWER_DOWN_CPU=y
```

`CONFIG_FREERTOS_USE_TICKLESS_IDLE` does not appear at all — its Kconfig depends
on `PM_ENABLE`, so it is not even a visible symbol. `CONFIG_FREERTOS_HZ=1000`.

## 3. Blast radius — what a PM overlay actually replaces

This is the part that explains the SD card, and it is bigger than `libesp_pm.a`.

arduino-esp32 2.x splits the SDK **two ways**, and a PM rebuild changes both:

    tools/sdk/esp32s3/lib/            98 archives, flash-mode independent
    tools/sdk/esp32s3/qio_opi/        libfreertos.a, libesp_system.a,
                                      libesp_hw_support.a, libspi_flash.a,
                                      libbootloader_support.a, sections.ld

The board's `build.arduino.memory_type` is `qio_opi`, so **that** directory is
the live one — and it holds FreeRTOS, esp_system and esp_hw_support, i.e. every
component `CONFIG_PM_ENABLE` and `CONFIG_FREERTOS_USE_TICKLESS_IDLE` change the
ABI of. The other five variant dirs (`dio_qspi`, `opi_opi`, …) are inert here.

**A partial overlay is therefore worse than no overlay.** Drop a PM-built `lib/`
in without the matching `qio_opi/` (or vice versa) and you get a binary whose
FreeRTOS and whose drivers disagree about tickless idle, interrupt allocation
and critical sections — which links cleanly and then misbehaves at runtime in
ways that look like anything but a power-management problem. A `SD.begin()` that
fails is exactly that shape: `SD` goes through `libdriver.a`'s SPI master, whose
DMA and interrupt setup lives in `libesp_hw_support.a`, in the variant dir.

`.github/workflows/build-pm-libs.yml` also required two edits *inside* the
framework to link at all — strip the pruned libs from the hardcoded link list in
`tools/platformio-build-esp32s3.py`, and remove
`-Wl,--wrap=esp_log_write/esp_log_writev/log_printf`. So the build script is a
third modified file, outside both lib directories. Its hash is in §1 for that
reason.

## 4. Reverting

**Do not repair the framework in place.** A hand-edited package that has been
partially restored is indistinguishable from a good one by every check
PlatformIO performs. Delete and re-download.

```sh
rm -rf ~/.platformio/packages/framework-arduinoespressif32
rm -rf .pio                              # build cache AND libdeps — both
pio pkg install -e t5s3-painter
tools/check-framework.sh                 # must print PASS
pio run -e t5s3-painter                  # must succeed
```

`rm -rf .pio` is not optional. It holds `libdeps/` as well as the object cache,
and a stale object built against PM headers will link happily into a stock
build.

If that still misbehaves, the platform itself may be involved:

```sh
pio pkg uninstall -p espressif32@6.5.0 && pio pkg install -e t5s3-painter
```

## 5. Before testing PM again — checklist

1. `tools/check-framework.sh` → **PASS**. Record the git commit you are on.
2. Work on branch `cpu-light-sleep` (the previous attempt is already there).
   `power-quick-wins` holds the unmerged app-level savings — BLE idle conn
   params and UI wake-on-input — and was itself built by the corrupted
   toolchain, so it needs rebuilding before its numbers mean anything.
3. **Overlay both `lib/` and `qio_opi/` from the same lib-builder run.** Never
   mix. Verify with `tools/check-framework.sh --print` that *both* tree hashes
   moved together.
4. Keep a copy of the stock framework before overlaying:
   `cp -a ~/.platformio/packages/framework-arduinoespressif32{,.stock}` — a
   local copy beats a 200 MB re-download when iterating.
5. **First test is `SD.begin()`, not power.** If the card does not mount, stop
   and revert; do not spend hours on the SD card. It is the canary for a
   mismatched overlay, not a storage bug.
6. Second test is the e-paper image. The known failure of the previous attempt
   was framebuffer corruption — the buffers live in OPI PSRAM and light sleep
   disturbs them — plus two panics, one in the `ui` task, immediately after
   `esp_pm_configure(...) -> ESP_OK`. That is the real open problem; the config
   is solved.
7. Only then measure. The rig already exists: the `battery:` line every 2 min in
   `/diag.log`, and the `power` console command. Baseline to beat is **167 mA
   idle / 183 mA session mean** (see `battery-life.md`).
8. **CI is the reference build.** `.github/workflows/build.yml` produces a
   firmware artifact from a clean environment. When on-device behaviour is
   inexplicable, flash the CI artifact before theorising — one flash eliminates
   the entire "is my toolchain lying to me" category.

## 6. Loose end

`power_mgmt.h`, `power_mgmt.cpp`, the runtime `pm:` log line and
`sdkconfig.defaults.pm` all cite **`investigations/archive/cpu-sleep-spike.md`**, which is not in the
main tree — the only copy is under `.claude/worktrees/agent-a5d264ba6aeddbd5f/`.
Four dangling references to the document that explains the problem. Restore it
or repoint them at this file and `battery-life.md`.
