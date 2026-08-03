# Battery-Life Analysis — Open E-Paper Bike Computer (LilyGO T5S3 4.7" PRO)

Firmware `v0.84`, ESP32-S3 @ 240 MHz, 1500 mAh cell. Analysis of the real ride-day
diagnostic log `bikegps-diag 6.log` (623 lines, 108 battery samples over 3.73 h)
plus a read of the `src/` power consumers.

---

## 1. Log analysis (measured)

### Discharge rate & runtime
The device charged/topped-off for the first ~1 h, then discharged continuously from
`01:03` to the end of the log.

| Metric | Value |
|---|---|
| Discharging samples (I<0) | 84 of 108 |
| Mean discharge current | **−181 mA** (median −182, σ 11.6) |
| Active-riding mean | −185 to −190 mA |
| Idle (GPS searching / post-ride, no active ride) | **−162 to −166 mA** |
| SOC slope | 100 % → 67 %, i.e. **−12.2 %/h** |
| Fuel-gauge coulomb slope | 1495 → 1002 mAh = **−182 mAh/h** |
| Voltage sag | 4109 mV (100 %) → 3763 mV (67 %) — healthy Li-ion curve, no brownout |

**Estimated runtime from full (1500 mAh):**
- To 0 %: `1500 / 181 ≈ ` **8.2 h**
- Usable (100 %→~5 %): **~7.4 h**
- Coulomb-based agrees: `1500 / 182 ≈ 8.2 h`

So this build gets roughly **8 hours** of GPS-recording ride time — modest for a 1500 mAh
cell; the drain is high and dominated by a large *baseline*, not by peaks.

### Phase breakdown

| Window | n | Mean mA | Notes |
|---|---|---|---|
| Pre-ride, GPS searching `01:03–01:10` | 4 | −162 | no ride, no HR, phone connected |
| Ride start + HR connect `01:10–01:21` | 6 | −187 | recording begins `01:10`, HR up `01:11` |
| Riding `01:21–02:13` | 26 | −190 | peaks to −214 |
| Riding `02:13–03:03` | 25 | −182 | |
| Ride end + FIT download `03:03–03:20` | 8 | −180 | ride saved 28.84 km / 7817 s at `03:20:51` |
| Post-ride idle `03:21+` | 13 | −166 | recording stopped, HR dropped |

**Key structural finding: the baseline is ~165 mA and active riding only adds ~20–25 mA.**
The device is expensive *just sitting there powered on* — the levers that matter are the
always-on consumers (backlight, CPU/BT, GPS), not the per-second refresh.

### Anomalies
- **Unreliable `charging`/`discharging` word.** The fuel-gauge `getIsCharging()` flag
  (`main.cpp:65,83`) is noisy: many samples read "charging" while current is clearly
  negative, e.g. `01:03 … -174mA charging`. **Trust the current sign, not the word.**
  (The log format should print the state from `ma < 0`, not the gauge flag.)
- **Three unexplained power-on resets in the first hour** — boots at log lines 47/120/169,
  all `reset: power-on / power-loss [1]` (`ESP_RST_POWERON`), with **no preceding
  `shutdown:` line**. These are during bench setup/charging (there's also a deliberate
  `gps power-cycle test` at `00:13` and an `auto-sleep` at `00:54`), so likely the battery
  connector / USB being handled — but a true brownout would look identical. Worth a
  glance; not implicated in ride drain.
- **Current spikes to −214 mA** at `01:39` and several −200+ readings while riding —
  consistent with the periodic GL16 full-refresh (every 24 DU), map redraws, and SD FIT
  flushes coinciding. Brief; small energy.
- **BLE link instability:** repeated `reason 520` (supervision timeout) disconnects, with a
  reconnect storm at `02:38–02:40` (8 reconnects in 2 min). Each reconnect re-runs
  `updateConnParams` and restarts advertising — minor power, but a symptom of the
  aggressive 30 ms / latency-0 link (see §3).

---

## 2. Power budget — apportioning the ~165 mA baseline

No per-rail instrumentation exists in the log, so this is a datasheet-plus-measured-delta
estimate. The firm anchors are: idle baseline ≈ **165 mA**, active adds ≈ **20 mA**, and
the idle→active delta is small.

| Subsystem | Est. avg mA | Confidence | Reasoning |
|---|---|---|---|
| **Backlight (front LED)** | **25–55** | **medium/high** | Default level is **2 of 3** (`settings.cpp:13 bl=2`), PWM **110/255 ≈ 43 %** into the PT4103 boost driver (`config.h:39`, `ui_dashboard.cpp:87,91`). On an e-paper device this is *always on unless the rider turns it off*, and is a prime suspect for why baseline is 165 mA and not ~110. |
| **ESP32-S3 @ 240 MHz + BT, never light-sleeps** | 55–80 | medium | Every task busy-polls: UI loop 30 ms (`ui_dashboard.cpp:1313`), GPS 50 ms (`gps_service.cpp:718`), BLE server 100 ms, sensors 1 s. FreeRTOS idle almost never runs, so the core stays at 240 MHz with the BT controller active. |
| **GPS (CASIC receiver + 3V3 rail)** | 20–30 | high | Rail (`IOEXP_PIN_RADIO_POWER`) is driven HIGH at boot (`main.cpp:164`) and **held on continuously**; only toggled by the power-cycle test and shutdown. A CASIC receiver tracking draws ~20–30 mA. |
| **E-paper refresh (1 Hz DU)** | 10–20 | medium | Full re-render + DU push once/sec while values change (`ui_dashboard.cpp:1198`), panel powered off between passive updates (`refresh()` `epd_poweroff` at :252). ~0.3 s of panel draw per second. Present in *both* idle and active baselines. |
| **BLE: phone link + advertising + sensor scan** | 8–18 | medium | Phone link at 15–30 ms, latency 0 (`ble_server.cpp:576`); active scan 30 ms/100 ms = 30 % duty while it runs. See §3 for the scan-forever bug. |
| **LoRa (SX126x on shared rail, unused)** | 1–5 | low | Rail powers LoRa too; chip is only CS-deselected (`ride_recorder.cpp:232`), **never put to sleep** — idles at a few mA. |
| **Fuel gauge / RTC / IO-expander / touch (I2C)** | 2–5 | low | small housekeeping. |

Rough sum ≈ 120–210 mA, centering on the observed ~165 mA. The two dominant, *avoidable*
chunks are **backlight (if left on)** and **CPU/BT never sleeping**.

---

## 3. Ranked recommendations

Ordered by value ÷ risk. Savings are estimates against the ~181 mA active baseline; each is
independently testable by logging mean current before/after over a fixed window (the log
already prints signed mA every 2 min — that is the measurement rig).

### R1 — Stop the all-ride active BLE scan for unpaired sensors ✅ *(drafted, see §4)*
**Save ~5–12 mA (~3–6 % → +0.2–0.5 h). Risk: very low. High confidence.**
`ble_sensors.cpp:354` computes `allConnected` over **all three** sensor kinds (HR / Power /
Cadence). If the rider only paired an HR strap (as in this log — HR connects `01:11`, power
& cadence never), Power and Cadence can never connect, so `allConnected` stays false and —
because `ride_recorder::isRecording()` holds `wantScan` true (`:364`) — the radio
**active-scans at 30 % duty for the entire 2 h 11 m ride** hunting for sensors that don't
exist. The code comment even warns about this case but the check doesn't honor it.
*Fix:* only let *paired* kinds (non-empty `settings::sensorAddr(k)`) gate scanning. Once the
paired HR is up, scanning stops. **Testable:** compare mean mA during recording with only an
HR strap paired, before vs. after.

### R2 — Default the backlight OFF (or auto-off in daylight)
**Save ~25–55 mA (~15–30 % → +1.5–3 h) if the rider had it on. Risk: low (UX/config).**
Default level is **2** (`settings.cpp:13`). E-paper is readable in daylight without a
frontlight; a bike computer should default to **0 (off)** and let the rider bump it. This is
the single largest potential win but it's a *policy* change (the rider may want the light),
so it's R2 not R1. Cheap variants: default to level 1, or add an auto-off timeout that drops
the backlight after N seconds of no touch (like the display idle logic already does).
**Testable:** log mean mA at each of the 4 levels for 2 min each.

### R3 — Relax the phone BLE connection parameters when idle
**Save ~3–10 mA. Risk: low–medium. Medium confidence.**
`ble_server.cpp:576` requests 15–30 ms interval, **latency 0**, for *every* connection to
keep bulk transfers (ride download / OTA) fast. But the steady state is a **1 Hz status
notify** (`ble_server.cpp:1156`) — 30 ms/latency-0 forces both the ESP32 and the phone to
wake ~33×/s for nothing. *Fix:* after a transfer completes (or if none is active), call
`updateConnParams` with a longer interval + non-zero slave latency (e.g. 30–60 ms, latency
4 → effective ~150 ms wake), and tighten back to fast params at the start of a
download/OTA. This may also calm the `reason 520` reconnect storm. **Risk:** iOS caps
latency and can renegotiate; test that downloads/OTA still run at full speed.

### R4 — Duty-cycle / idle the GPS rail when stopped and not recording
**Save up to ~20–30 mA while parked; ~0 while riding. Risk: medium.**
The GPS rail is on 100 % of the time (`main.cpp:164`). While *not recording and stopped*,
the receiver doesn't need continuous 1 Hz fixes. Options, cheapest first: (a) rely on the
existing 10-min auto-sleep (`ui_dashboard.cpp:1082`) — already good; (b) put the CASIC into
its low-power/periodic mode when stopped; (c) power the rail down between fixes. **Caveat:**
we already tuned this rail for first-fix time — cutting power loses the hot-start RAM and
hurts TTFF, so any duty-cycling must keep backup power or only engage when clearly idle.
Net ride savings are small (you need GPS while riding); the win is a parked device.
Also cheap: explicitly put the unused **LoRa SX126x to sleep** at boot (~1–5 mA).

### R5 — Light-sleep the CPU between 1 Hz work
**Potentially save ~20–40 mA. Risk: medium–high. Lower confidence.**
Every task busy-polls on short `vTaskDelay`s, so the core never idles enough to clock down.
Automatic light-sleep (`esp_pm_config` with `light_sleep_enable`) could gate the core
between BLE/GPS events, but ESP32-S3 automatic light sleep with an **active BT connection
and OPI PSRAM at 120 MHz** is delicate, and the UI task's 30 ms poll would defeat it anyway.
Would require reworking UI/touch to be event-driven (interrupt-woken) rather than polled.
High effort, real payoff, but the riskiest — do last, behind R1–R4.

### R6 — CPU downclock to 160 MHz — ❌ do NOT (documented hazard)
`main.cpp:137–142` records that a 160 MHz downclock was **removed** because the OPI PSRAM
panicked at the lower clock and the device boot-looped. Leave at 240 MHz unless octal-PSRAM
stability at 160 MHz is proven first. Listed only to close the loop.

### Recommendation summary

| # | Change | Est. save | Risk | Effort |
|---|---|---|---|---|
| R1 | Don't scan for unpaired sensors mid-ride *(drafted)* | 5–12 mA | very low | tiny |
| R2 | Backlight default OFF / auto-off | 25–55 mA | low (UX) | tiny |
| R3 | Relax phone conn params when idle | 3–10 mA | low–med | small |
| R4 | GPS rail idle when parked + sleep LoRa | ≤20–30 mA parked | med | med |
| R5 | CPU light-sleep between events | 20–40 mA | med–high | large |
| R6 | ~~160 MHz downclock~~ | — | unsafe | — |

Doing **R1 + R2 + R3** alone plausibly moves active drain from ~181 mA toward ~120–140 mA,
i.e. runtime from ~8 h toward **~10–12 h**, with little risk.

---

## 4. Drafted change (uncommitted, NOT flashed): R1

`src/ble_sensors.cpp`, in `task()` — the `allConnected` computation now only counts *paired*
sensor kinds, so an unpaired Power/Cadence kind no longer pins `wantScan` true for the whole
ride:

```cpp
// "All connected" means every PAIRED sensor is connected. A kind the
// user never paired (no saved address) can never connect, so counting
// it here would leave allConnected=false forever and keep the radio
// active-scanning for the entire ride (recording holds wantScan true) —
// burning power hunting for a power meter / cadence sensor that doesn't
// exist. Only paired kinds gate scanning; once they're up, scanning stops.
bool allConnected = true;
for (int k = 0; k < KIND_COUNT; ++k) {
    bool paired = settings::sensorAddr(k)[0] != 0;
    if (paired && !sensors[k].connected) allConnected = false;
}
```

- **Why safe:** purely narrows *when* the radio scans; no change to pairing, connection, or
  data flow. A rider with all sensors paired sees identical behavior. Scanning still resumes
  briefly if a paired sensor drops (`onDisconnect` clears `connected`) or during the Sensors
  screen / 30 s post-interaction window.
- **Verify before flashing:** confirm `settings::sensorAddr(int)` is safe to call from the
  BLE task each second (it reads NVS-backed cached strings — cheap; used identically at
  `ble_sensors.cpp:214`). Then measure mean mA during a recording with only an HR strap
  paired, before vs. after.

_Left uncommitted; nothing flashed._
