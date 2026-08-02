# How to undo the PM (light-sleep) framework experiment

**If builds are broken, the SD card won't mount, or the panel is corrupt — you
are probably on the PM framework. Do this:**

```sh
cd ~/Documents/tdisplay
tools/revert-pm.sh
```

That's it. It restores the stock framework, clears every build cache, and
verifies the result. Read on only if you want to know what it does or it fails.

---

## What was changed — applied 2026-07-31

Exactly **two paths**, both inside the PlatformIO package directory. Nothing
else in your system, and nothing in the project.

**1. The whole S3 SDK tree, replaced wholesale:**

    ~/.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32s3/

The `build-pm-libs.yml` artifact (`esp32s3-arduino-libs-pm`, run 30173426845,
2026-07-25) turned out to contain the *entire* tree — `bin/ include/ ld/ lib/
sdkconfig` and all six flash-mode variant dirs — so this was a full replace, not
an overlay. **The partial-overlay hazard described in `pm-rebuild-baseline.md`
§3 therefore does not apply to this attempt.** Verified before installing:

| Check | Result |
|---|---|
| `CONFIG_PM_ENABLE` | `=y` |
| `CONFIG_FREERTOS_USE_TICKLESS_IDLE` | `=y` |
| `qio_opi/include/sdkconfig.h` → `CONFIG_SPIRAM_MODE_OCT` | `1` — OPI PSRAM preserved |
| ESP-IDF version | 4.4.6 — same as stock |
| CPU freq / PSRAM speed | 240 MHz / 80 MHz — unchanged |
| `esp_pm_configure` in `libesp_pm.a` | 223 bytes (stock: 8-byte stub) |

**2. The build script, patched:**

    ~/.platformio/packages/framework-arduinoespressif32/tools/platformio-build-esp32s3.py

Two edits, both forced by the component pruning that made the lib-builder run
succeed in the first place:

- Removed `-Wl,--wrap=esp_log_write`, `--wrap=esp_log_writev`,
  `--wrap=log_printf` — those wrappers live in `libesp_diagnostics.a`, which the
  PM build prunes, so the link fails on an undefined wrapper.
- Removed 19 `-l` entries for archives the PM build no longer ships:
  `cat_face_detect color_detect dl esp32-camera esp_diagnostics esp_insights
  esp_rainmaker esp_schedule espressif__esp-dsp espressif__esp_secure_cert_mgr
  gpio_button human_face_detect json_generator json_parser mfn qrcode
  rmaker_common rtc_store ws2812_led` — camera, esp-dl, esp-dsp and rainmaker
  plus its dependencies. We link none of them.

A copy of the original sits next to it as `platformio-build-esp32s3.py.stock`,
but the full-package restore below supersedes it — use that.

**No project files were changed to enable PM.** `platformio.ini`, `src/`, the
board JSON and `vendor/` are untouched — `power_mgmt.cpp` already contained the
runtime wiring and has been a logged no-op all along. So reverting is purely a
matter of putting the framework back; there is nothing to `git checkout`.

### Result of the build

Links clean. Flash 18.4% (1,206,273 bytes, +6 KB vs stock), RAM 41.8%. The
firmware now contains real implementations where it previously had stubs:

    esp_light_sleep_start   714 bytes    <- linked in
    vApplicationSleep       266 bytes    <- the tickless-idle hook
    esp_pm_configure        219 bytes    (was 8)
    esp_pm_lock_create       87 bytes    (was 8)

## Revert, option 1 — the local backup (fast, ~10 s)

A byte-for-byte copy of the stock framework was taken before the overlay:

    ~/.platformio/packages/framework-arduinoespressif32.stock

```sh
rm -rf  ~/.platformio/packages/framework-arduinoespressif32
cp -a   ~/.platformio/packages/framework-arduinoespressif32.stock \
        ~/.platformio/packages/framework-arduinoespressif32
rm -rf  ~/Documents/tdisplay/.pio
cd ~/Documents/tdisplay && tools/check-framework.sh    # must print PASS
pio run -e t5s3-painter                                # must succeed
```

`rm -rf .pio` is **not optional** — it holds `libdeps/` as well as the object
cache, and a stale object compiled against PM headers will link happily into a
stock build and then misbehave at runtime.

## Revert, option 2 — re-download (if the backup is gone or suspect)

```sh
rm -rf ~/.platformio/packages/framework-arduinoespressif32
rm -rf ~/Documents/tdisplay/.pio
cd ~/Documents/tdisplay && pio pkg install -e t5s3-painter
tools/check-framework.sh
pio run -e t5s3-painter
```

Takes a few minutes (749 MB package). Slower, but it cannot inherit a mistake.

## Revert, option 3 — nuclear (only if 1 and 2 both fail)

```sh
rm -rf ~/.platformio/packages/framework-arduinoespressif32*
rm -rf ~/.platformio/platforms/espressif32@6.5.0
rm -rf ~/Documents/tdisplay/.pio
cd ~/Documents/tdisplay && pio pkg install -e t5s3-painter
```

Note the `*`: there are three framework slots on this machine. The other two
(`@src-533e72d0…` and `-libs`) belong to the separate ESP-IDF-port experiment on
arduino-esp32 3.3.11 / IDF 5.5 and are not used by `t5s3-painter`. Removing them
is harmless; they will be re-fetched if something needs them.

## Never repair the framework in place

Do not try to restore individual `.a` files, re-extract part of an archive, or
"put back just the one that broke". A partially repaired package is
indistinguishable from a good one by every check PlatformIO performs — the
directory has **no version suffix in its name**, so a modified copy keeps
reporting `3.20014.231204` forever. That is exactly how a previous attempt
produced a toolchain where `SD.begin()` failed on *every* local build, including
commits from two weeks earlier, while a version audit said everything matched
CI. Delete the whole directory and start from a known source.

## How to tell which framework you are on

```sh
tools/check-framework.sh --fast
```

The decisive line needs no recorded hash. On the **stock** framework
`esp_pm_configure` is an 8-byte stub that returns `ESP_ERR_NOT_SUPPORTED`:

    ok    esp_pm_configure is a 8-byte stub (PM compiled out — stock)

On the **PM** framework it is a real function of several hundred bytes:

    DIFF  esp_pm_configure is NNN bytes — this is a PM-ENABLED framework

At runtime the device tells you too, in `/diag.log` on every boot:

    pm: light sleep UNAVAILABLE — ...            <- stock
    pm: esp_pm_configure(min=max=240, ...) -> ESP_OK   <- PM

## Symptoms that mean "revert now, don't debug"

From the previous attempt. None of these look like a power-management problem,
which is why they cost hours:

| Symptom | What it actually is |
|---|---|
| `SD.begin()` fails; `cardType=NONE`; survives a revert | **NOT the overlay — do not revert for this.** 2026-08-02: a wedged card controller, caused by abrupt resets landing mid-SPI-write. Anything that persists *after* the framework is restored cannot be caused by the framework. `cardType=NONE` means the card is not answering at the protocol level, so the filesystem is irrelevant. Reformat the card. See `battery-life.md`, "A day lost to a self-inflicted fault" |
| E-paper image corrupt, ghosting, garbage | framebuffers live in OPI PSRAM; light sleep disturbs them. **This is the known open blocker, not a mistake in your overlay** |
| Panic in the `ui` task shortly after boot | same PSRAM/light-sleep interaction; two of the only two real panics in the logs came from this build |
| Link errors about `esp_log_write` / `log_printf` wrappers | `-Wl,--wrap=` flags in `tools/platformio-build-esp32s3.py` referencing a pruned component |

The first one means your overlay is wrong. The middle two mean the overlay is
*right* and you have reached the actual unsolved problem.

## After reverting

Baseline to confirm you are back to normal — `167 mA` idle / `183 mA` session
mean, `/diag.log` `battery:` line every 2 minutes. Firmware v0.86 on `main`
@ `d08d88d` is known good and flashed.

Background: [`investigations/pm-rebuild-baseline.md`](investigations/pm-rebuild-baseline.md)
(version table, content hashes, blast radius) and
[`investigations/battery-life.md`](investigations/battery-life.md) (why any of
this is worth doing).
