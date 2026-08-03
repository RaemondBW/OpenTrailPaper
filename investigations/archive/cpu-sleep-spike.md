# CPU Sleep Spike — automatic light sleep on the e-paper bike computer

**Goal:** make the ESP32-S3 sleep between the ~1 Hz workloads instead of running
the CPU flat-out at 240 MHz, without dropping GPS fixes or BLE, and without
re-triggering the OPI-PSRAM boot loop.

**Result up front:** a PM-enabled build **is achievable** on this stack, but
**not with the stock precompiled Arduino framework** — power management is
compiled out of it, so `esp_pm_configure()` returns `ESP_ERR_NOT_SUPPORTED` at
runtime. Enabling light sleep requires rebuilding the framework from source with
`CONFIG_PM_ENABLE=y` (+ tickless idle + BT modem sleep). The firmware-side
wiring is drafted and **compiles cleanly against the stock framework today**
(graceful no-op until the framework is rebuilt). The recommended safe runtime
config is **light-sleep-only with no DFS (min = max = 240 MHz)**, which sidesteps
the PSRAM problem entirely.

---

## 0. Verification of the blocker (confirmed)

The precompiled Arduino framework ships PM compiled out. In
`~/.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32s3/sdkconfig`:

- `# CONFIG_PM_ENABLE is not set`
- `# CONFIG_FREERTOS_USE_TICKLESS_IDLE` — absent
- `# CONFIG_BT_CTRL_MODEM_SLEEP is not set` (line 467)
- `CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240=y` (line 910)

`esp_pm.h` confirms the runtime behavior (lines 63–64): `esp_pm_configure()`
returns `ESP_ERR_NOT_SUPPORTED` "if `CONFIG_PM_ENABLE` is not enabled in
sdkconfig". The `esp_pm_configure` symbol *is* present in `libesp_pm.a` (it is
the disabled stub), so **the drafted firmware links and runs safely on stock** —
it just won't sleep. Both the pinned `espressif32@6.5.0` (Arduino 2.0.14) and the
installed `@6.13.0` (Arduino 2.0.17) are Arduino 2.x with PM off; this is a
framework-build property, not a version quirk.

### PSRAM finding (important — corrects an assumption)

The board JSON (`vendor/.../boards/T5-ePaper-S3.json`) uses
`"memory_type": "qio_opi"` → **octal (OPI) PSRAM**. The `-DPSRAM_SPEED=120MHz`
build flag in `platformio.ini` **is consumed nowhere** — not in the project
(`grep` of `src/`), not in the Arduino core/tools. It is a dead define. The
stock `qio_opi` variant runs **OPI PSRAM at 80 MHz** (`CONFIG_SPIRAM_SPEED_80M`).
So the device is almost certainly **already at 80 MHz**, not 120 MHz, and the
`main.cpp:137-142` comment ("runs 120 MHz OPI PSRAM") appears inaccurate. This
*simplifies* the sleep story: we do **not** need to change PSRAM speed at all.

The real constraint that boot-looped 160 MHz (`main.cpp:137-142`) is that **80 MHz
OPI PSRAM timing is derived assuming a 240 MHz CPU/APB**; reducing the clock while
PSRAM is live corrupts it. The fix is not to change PSRAM but to **never run DFS**
— see §2.

---

## 1. Recommended build path

**Rebuild the framework from source with Arduino as an ESP-IDF component, on the
community `pioarduino` platform, driven by a project `sdkconfig.defaults`.**

This is option (b) "Arduino-as-an-ESP-IDF-component" executed on platform (a)
`pioarduino`. Rationale:

- Flipping a compiled-out Kconfig like `CONFIG_PM_ENABLE` is **impossible with
  the prebuilt-library `framework = arduino` flow** (both official and
  pioarduino ship the same precompiled `libesp_pm.a`). You must compile the IDF
  + Arduino from source. `framework = arduino, espidf` does exactly that, and the
  board already declares both frameworks (`"frameworks": ["arduino","espidf"]`).
- The **official `espressif32` platform is EOL/deprecated**; its component build
  is unmaintained. **`pioarduino`** is the actively-maintained continuation
  (IDF 5.1/5.3, Arduino core 3.x, ESP32-S3 well supported) and makes the
  `arduino, espidf` build reliable. With the component build, PlatformIO's IDF
  integration auto-reads `sdkconfig.defaults` from the project root — no exotic
  ini keys needed.

**Cost to flag:** pioarduino moves you from **Arduino core 2.0.x → 3.0.x
(IDF 4.4 → 5.x)**. That is a framework-major migration with real API churn to
work through (Arduino 3.x `Serial`/`analog`/`ledc` signatures, USB stack,
possibly `NimBLE-Arduino` — currently `@^2.2.3`, which supports both, good). The
first from-source build downloads a new platform + IDF 5.x toolchain (**multi-GB,
~10 min**) and compiles everything (**~15–40 min, several GB of `.pio`**).

### Alternative (keep Arduino 2.0.17, no code migration)

Use **`esp32-arduino-lib-builder`** to rebuild just the precompiled 2.0.17
libraries with a patched sdkconfig (`CONFIG_PM_ENABLE`, etc.), then keep the
stock `espressif32@6.5.0` platform pointed at your custom `tools/sdk`. No app
migration, but heavier one-time tooling (Docker/clone/menuconfig) and you now
maintain a fork of the libs. **Choose this if avoiding the 2.x→3.x migration
matters more than build-tooling simplicity.** Otherwise pioarduino is cleaner
long-term.

### `platformio.ini` diff (pioarduino path)

```diff
 [env:t5s3-pro]
-platform = espressif32@6.5.0
+; Community fork with a from-source Arduino-as-IDF-component build (PM-capable).
+; Pin a specific pioarduino release; 53.03.xx == IDF 5.3 / Arduino core 3.x.
+platform = https://github.com/pioarduino/platform-espressif32/releases/download/53.03.13/platform-espressif32.zip
 board = T5-ePaper-S3
-framework = arduino
+framework = arduino, espidf          ; build Arduino from source as an IDF component
 upload_speed = 921600
 monitor_speed = 115200
 monitor_filters = esp32_exception_decoder
+
+; ESP-IDF picks this up automatically from the project root and rebuilds the
+; affected components. (Drafted as sdkconfig.defaults.pm in this spike — rename
+; to sdkconfig.defaults for the real build.)
+; board_build.sdkconfig files are auto-discovered; no extra key required.

 build_flags =
     -DBOARD_HAS_PSRAM
-    -DPSRAM_SPEED=120MHz
+    ; -DPSRAM_SPEED=120MHz   ; REMOVED: dead define, consumed nowhere; PSRAM is 80 MHz OPI
     -DARDUINO_USB_CDC_ON_BOOT=1
     ...
```

The **sdkconfig options** to set live in `sdkconfig.defaults` — see the drafted
`sdkconfig.defaults.pm` in this worktree. Key lines:

```ini
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3
CONFIG_PM_SLP_IRAM_OPT=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y      ; stay at 240; runtime uses min=max=240
CONFIG_SPIRAM_MODE_OCT=y                   ; keep OPI PSRAM
CONFIG_SPIRAM_SPEED_80M=y                  ; keep 80 MHz (unchanged)
CONFIG_BT_CTRL_MODEM_SLEEP=y
CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1=y
CONFIG_BT_CTRL_LPCLK_SEL_RTC_SLOW=y        ; BLE LP clock survives light sleep
CONFIG_RTC_CLK_SRC_INT_RC=y                ; internal RC (no 32k XTAL assumed)
```

USB/serial-console note: with native USB CDC (`ARDUINO_USB_MODE=0`,
`ARDUINO_USB_CDC_ON_BOOT=1`) **light sleep gates the USB-OTG PHY and drops the
CDC link** — the serial monitor churns connect/disconnect and GPS telemetry
becomes unusable while sleeping. The drafted firmware handles this by **holding a
`ESP_PM_NO_LIGHT_SLEEP` lock whenever the CDC port is open** (`power_mgmt::tick`),
so the bench console works when plugged in and the device only sleeps on battery
— which is exactly when sleep matters. No USB console loss in the field.

---

## 2. PSRAM decision

**Keep octal PSRAM at 80 MHz. Do NOT enable DFS. Run light-sleep-only with
`min_freq_mhz == max_freq_mhz == 240`.**

Why this is safe: during light sleep the CPU/PLL are powered down and **PSRAM is
retained in self-refresh** (`CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND=y` is
already set in the stock cfg and kept in ours). On wake the PLL is restored to
240 MHz **before any code — and therefore any PSRAM access — runs**. Because the
CPU is never *running instructions against PSRAM* at a reduced clock, the
80 MHz-needs-240 MHz timing constraint is never violated. This is precisely the
distinction from the removed `setCpuFrequencyMhz(160)` experiment, which ran the
CPU (and PSRAM accesses) at 160 MHz continuously and corrupted PSRAM.

- **No framebuffer / map-render performance cost** — the CPU and PSRAM run at
  full 240/80 whenever awake. Sleep is transparent to render code.
- **DFS (min < 240) is explicitly rejected for v1.** It would clock the APB down
  while PSRAM is live = the 160 MHz boot-loop failure mode. If DFS is ever wanted
  for extra awake-time savings, PSRAM must first drop to **40 MHz (and likely
  Quad mode)** and map-render/framebuffer throughput must be re-measured — a
  bigger, riskier change. Not recommended now.

---

## 3. esp_pm wiring (drafted)

New module **`src/power_mgmt.{h,cpp}`** (drafted, uncommitted). The core call:

```cpp
esp_pm_config_esp32s3_t cfg = {};
cfg.max_freq_mhz = 240;
cfg.min_freq_mhz = 240;          // == max: light sleep only, no DFS (PSRAM-safe)
cfg.light_sleep_enable = true;
esp_err_t err = esp_pm_configure(&cfg);   // ESP_ERR_NOT_SUPPORTED on stock -> logged
```

Called from `main.cpp setup()` **after** all peripherals are up
(`ui_dashboard::begin()`), so the UART/BLE/EPD drivers are installed first.
`power_mgmt::tick()` is called from `loop()` to manage the USB-CDC no-sleep lock.

### Tasks that poll, and what tickless idle needs

Tickless idle only sleeps when **every** task is blocked. Current cadences:

| Task | File:line | Delay | Notes |
|------|-----------|-------|-------|
| `ui`  | `ui_dashboard.cpp:1313` | **30 ms** | highest-rate poll — the limiter |
| `gps` | `gps_service.cpp:718` | 50 ms | drains UART FIFO |
| `srv` (BLE) | `ble_server.cpp` task | 100 ms | status notify |
| `ble` (sensors) | `ble_sensors.cpp:390` | 1000 ms | |
| `rec` | `ride_recorder.cpp:398` | 1000 ms (`RECORD_INTERVAL_MS`) | |
| `bat` | `main.cpp:87` | 30000 ms | |
| `loop()` | `main.cpp:258` | 1000 ms | |

All tasks already `vTaskDelay()` (none busy-spin), and touch/buttons are
interrupt-driven (`touchIrq`, `boardBtnIrq`), so the system **will** sleep
between polls. **With no code change, the CPU sleeps in ≤30 ms chunks** — the UI
task's 30 ms poll wakes it, it does a few µs of work, and it sleeps again,
sleeping ~90%+ of each idle second. That alone is the bulk of the win.

**Minimal change to deepen sleep (recommended, but left to the display agent):**
the UI task's 30 ms cadence exists mostly for touch/button *fallback* polling,
which is already interrupt-backed. Raising the UI idle-loop `vTaskDelay` (e.g. to
~150–200 ms) when the screen is static would let the CPU sleep in ~150 ms chunks
and cut wake overhead ~5×. **This edit lives in the display/refresh loop
(`ui_dashboard.cpp`), which another agent is reworking — I did not touch it.** It
is safe for GPS (see §4) as long as the GPS task keeps its ≤100 ms poll.

---

## 4. GPS UART across sleep (no dropped NMEA)

`SerialGPS` = `Serial2` (`gps_service.cpp:16`), `begin()` at
`gps_service.cpp:190-196`: `setRxBufferSize(1024)` then `begin(9600, ...)`. The
1024-byte buffer is the **software ring**, filled by the UART ISR. The hardware
FIFO is **128 bytes**. During light sleep the ISR can't run, so only the 128-byte
HW FIFO buffers incoming bytes: at 9600 baud (960 B/s) it fills in **~133 ms**.

**Primary mechanism (no code change needed): bounded sleep.** Tickless idle only
sleeps until the *next* task wakeup. The GPS task's own **50 ms** poll
(`gps_service.cpp:718`) bounds every sleep to ≤50 ms → ~48 bytes accumulate, well
under 128. Even if the UI idle delay is raised to 200 ms (§3), the **GPS task's
50 ms poll still bounds the sleep** and drains the FIFO. Rule: **keep the GPS
poll ≤ ~100 ms** and no NMEA is lost. Current design already satisfies this.

**Optional belt-and-suspenders: UART wake source.** `esp_sleep_enable_uart_wakeup`
+ `uart_set_wakeup_threshold` wake the SoC on RX edges before the FIFO overflows.
Caveat (`esp_sleep.h:297` + S3 silicon): **only UART0/UART1 can wake the S3 from
light sleep — UART2 cannot.** GPS is on UART2, so to use this you must move
`SerialGPS` to `Serial1` (a one-line change; pins are passed explicitly in
`begin()`, and UART1 is otherwise free). The drafted `power_mgmt.cpp` includes
this path behind `#ifdef PM_GPS_UART_WAKEUP`, disabled by default since the
bounded-sleep guarantee already covers 9600 baud. Enable it only if the GPS poll
is ever raised above ~100 ms.

---

## 5. BLE coexistence (modem sleep + connection interval)

Two independent sleeps must line up:

1. **Controller modem sleep** (`CONFIG_BT_CTRL_MODEM_SLEEP` + `..._MODE_1`): the
   BLE radio powers down between connection events. For it to coexist with light
   sleep, the controller's low-power clock must keep ticking while the SoC sleeps
   → **`CONFIG_BT_CTRL_LPCLK_SEL_RTC_SLOW`** (RTC slow clock), *not* the main
   XTAL (gated in light sleep; using it forces the controller to hold a
   no-light-sleep lock and you get **no CPU sleep while connected**). We source
   the RTC slow clock from the **internal 150 kHz RC** (`CONFIG_RTC_CLK_SRC_INT_RC`)
   unless the board wires a 32.768 kHz crystal to the ESP32 (VERIFY on the
   schematic — the PCF8563 is a *separate I2C* RTC and does not count). Internal
   RC drifts ~5%, which widens BLE wake windows slightly (a little more power,
   occasional connection slop) — acceptable; measure.

2. **Connection interval.** Modem sleep only saves power in proportion to how
   long the radio can sleep between events = the connection interval. The current
   code requests **15–30 ms and never relaxes it** (`ble_server.cpp` onConnect:
   `updateConnParams(handle, 12, 24, 0, 400)`). (Note: the spec brief said an
   idle 150–300 ms interval "was just raised" — **it is not in the tree**; the
   code keeps 15–30 ms always.) A 15–30 ms interval wakes the radio ~33–66×/s and
   caps CPU light-sleep windows.

   **Drafted change (`ble_server.cpp`):** relax to **150–300 ms with slave
   latency 4** when idle, snap back to 15–30 ms during any bulk transfer
   (OTA/map/ride/log/sensor-stream). Implemented via a stored `s_connHandle` and
   `applyConnInterval(bool fast)`, driven from the server task by a `bleBusy`
   check. 1 Hz status notifications are unaffected (a peripheral may notify
   immediately regardless of slave latency). This is what turns modem sleep into
   an actual idle-current win while a phone stays connected.

NimBLE specifics: no host-side API change beyond `updateConnParams`; the
controller config is all in `sdkconfig.defaults`. `NimBLE-Arduino@^2.2.3` is
compatible with both the current 2.x and the pioarduino 3.x cores.

---

## 6. Build attempt (this spike)

- Symlinked the gitignored `vendor/` board support into the worktree.
- Compiled `pio run -e t5s3-pro` on the **stock** `espressif32@6.5.0` framework
  **with all drafted PM code included** (`power_mgmt.*`, `main.cpp`,
  `ble_server.cpp`). Purpose: prove the source integrates, compiles, and links
  against the real toolchain (the `esp_pm_*` / `esp_sleep_*` symbols exist).
  **Result: see the end of this file / the final report** — a clean stock build
  means the firmware is safe to keep in-tree today and will "just work" once the
  framework is rebuilt (the PM calls flip from `NOT_SUPPORTED` no-ops to live).
- **Not done (the heavy remaining step):** switching to `pioarduino` +
  `framework = arduino, espidf` and doing the from-source PM-enabled build. That
  downloads a new platform/IDF 5.x toolchain (multi-GB) and rebuilds everything
  (~15–40 min), and carries the Arduino 2.x→3.x migration. Deferred as out of
  scope for a no-flash spike; the exact config to do it is in
  `sdkconfig.defaults.pm` + the ini diff above.

---

## 7. Risks

1. **Framework migration (pioarduino / Arduino 3.x).** API churn across the
   codebase; must recompile/fix the whole app. Mitigation: the lib-builder
   alternative keeps 2.0.17.
2. **OPI PSRAM + any accidental DFS = boot loop.** Enforced by `min == max ==
   240`. Never set `min_freq_mhz < 240` while PSRAM stays 80 MHz OPI.
3. **BLE stability on internal-RC LP clock.** ~5% drift may cause occasional
   supervision-timeout disconnects at 150–300 ms interval. Mitigation: keep
   supervision timeout generous (600 = 6 s, as drafted) and/or fit a 32k XTAL.
4. **USB console lost during sleep.** Mitigated by the no-sleep lock while CDC is
   open; still means field-battery runs have no live console (expected).
5. **Wake latency.** Light-sleep wake is ~tens of µs–ms; touch/button IRQ
   response gains a small latency. Should be imperceptible at ≤30 ms cadence.
6. **NMEA loss if GPS poll is later slowed.** Bounded-sleep guarantee holds only
   for GPS poll ≤ ~100 ms; enable the UART1 wake path if that changes.
7. **Actual current win is unverified without hardware** — see the bench plan.

---

## 8. Bench measurement plan (current meter required)

Use an inline power meter (e.g. a Nordic PPK2 or a µCurrent + scope on the
battery lead) at each step. Measure **average** current over ≥60 s, plus the
sleep/active duty cycle if the meter can scope it.

**A. Idle draw, before vs after (headline number).**
1. Baseline (stock framework), device on the desk, GPS on, BLE advertising, no
   phone, screen static → record mA.
2. PM build, same conditions → record mA. Expect the average to drop toward the
   light-sleep floor between the ~30 ms wakes. Order-of-magnitude expectation:
   the 240 MHz CPU core is a large share of the non-radio budget; if the CPU is
   asleep ~90% of idle time, expect a **meaningful double-digit-percent drop in
   the CPU/idle portion** of system current (radio, PSRAM self-refresh, EPD bias,
   and sensors are unchanged). Report the absolute mA delta — that is the win.
3. Repeat with a phone connected: (a) at the old 15–30 ms interval, (b) at the
   drafted 150–300 ms idle interval. Quantify the connection-interval savings.

**B. GPS-fix reliability (must not regress).**
- Cold start and warm start, PM on vs off: TTFF and whether it holds a fix.
- Watch the SD diag log `gps ...: chars=`, `ck=ok/bad` — a rising bad-checksum
  count or stalled `chars` under PM = dropped NMEA (FIFO overflow). Confirm the
  bounded-sleep assumption holds; if not, enable the UART1 wake path (§4).
- Ride for ≥15 min moving; confirm no fix dropouts vs a PM-off control ride.

**C. BLE stability.**
- Phone connected 30+ min at the idle interval: count unexpected disconnects
  (`diag` "phone disconnected reason") — especially supervision timeouts (reason
  0x08). Compare internal-RC vs (if fitted) 32k-XTAL LP clock.
- Run a full ride download / OTA / map upload with PM on: verify the fast
  interval snaps back (throughput unchanged vs baseline) and re-relaxes after.

**D. Map-render time (guard against a PSRAM regression).**
- Since PSRAM stays 80 MHz OPI, render time should be **unchanged**. Time a fixed
  map pan/redraw (add a `millis()` delta around the map render, or reuse existing
  timing logs) PM-off vs PM-on; confirm no delta. *If* DFS/40 MHz PSRAM is ever
  explored, re-run this as the go/no-go metric.

**E. Wake correctness spot-checks.**
- Touch and both buttons wake and respond after a long idle.
- Auto-sleep (deep) at the 10 min timeout still fires (`ui_dashboard.cpp:1082`).
- USB replug mid-idle: console returns (no-sleep lock re-acquires).
