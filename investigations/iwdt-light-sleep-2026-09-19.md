
## 2026-09-20: another one, with the watchdog already at 1000 ms

Build Sep 19 12:20 (PR #79's stretch active — boot logged `stage0=2000 ticks
(1000 ms)`), interrupt-watchdog reset at 19:11 mid-ride, dump
`memfault-1013aeda…`, decoded against the exact ELF (sha 665bd5f0…).

| core 0 (task `gps`) | core 1 | sleep state |
|---|---|---|
| `_xt_lowint1` dispatcher (xtensa_vectors.S:1114), a0 = `i2c_isr_handler_default` i2c.c:553 — i.e. just returned from the I2C ISR into the level-1 dispatcher | `esp_pm_impl_waiti` (IDLE) | light sleep active: 636 calls/min, 18.5 s/min, `holders=prlx`, **no sensor links** ("sensor sleep" armed but HR/Power up=0) |

So the tripled budget did not help, and the I2C ISR is now in four of five
dumps. Two mechanisms remain, and the dump cannot tell them apart:

1. **Stall longer than the budget.** Each light-sleep call stalls core 0 for
   its whole length. With no sensor links the only 1 Hz wake is the GPS burst,
   so a single call can approach 1 s — over the 1000 ms budget if MWDT1 keeps
   counting through light sleep. `sleep_stats` records only the sum, not the
   longest call; that number would settle this.
2. **I2C interrupt storm after sleep.** The IDF I2C driver holds only an
   APB-frequency PM lock (i2c.c:314), not a no-light-sleep lock, and our
   `i2cLock()` holds no sleep lock either — so light sleep can gate the I2C
   peripheral mid-transaction (gauge poll, IO-expander button read, RTC). A
   peripheral that comes back with an un-clearable status re-asserts its
   level-1 interrupt forever; the dispatcher services ONE interrupt per entry,
   MSB first (xtensa_vectors.S `dispatch_c_isr`), so a storming I2C source
   with a higher interrupt number starves the tick ISR that feeds the
   watchdog. That is exactly "core 0 sitting in the dispatcher just after the
   I2C ISR, core 1 idle", and it does not care how long the budget is.

Both are cheap to address at once: hold light sleep off across every I2C
transaction (`i2cLock`/`i2cUnlock` → `busyAcquire`/`busyRelease`, as sdLock
already does), cap single sleep calls below the watchdog with a periodic
esp_timer wake, and log the longest sleep call per pm window.


## 2026-10-02: two more, same signature, with #89 and #93 on board

Ride 06:47–09:11, firmware v1.20 build "Sep 30 11:37:49" (build id d1f523b4,
no local ELF — framework IRAM symbols below are from a sibling build and are
stable across builds; app addresses were not resolved). Both resets were
`interrupt watchdog [5]`; both rides resumed via ride recovery.

| dump | time | core 0 | core 1 |
|---|---|---|---|
| db1e825a | 07:39:16 | pc `i2c_hal_get_intsts_mask` (i2c_hal_iram.c:73), a0 `i2c_isr_handler_default` (i2c.c:492), stack `_xt_lowint1`/`_frxt_int_enter`, current task **gps** | `esp_pm_impl_waiti` from the idle hook (light sleep) |
| f389cddb | 09:03:57 | pc `i2c_isr_handler_default` (i2c.c:492), a0 `_xt_lowint1`, stack `esp_pm_impl_isr_hook`, current task **gps** | `esp_pm_impl_waiti` from the idle hook |

Seconds before: (1) the phone link had just dropped to the 300 ms idle
interval and BT modem sleep came on; (2) a fuel-gauge poll (`battery:` line,
i2cLock-covered) logged 11 s earlier, sensor sleep had just gone OFF, pm
window reported 929 sleep calls/min. Neither tail shows a paint, so #89's
paint hold was not what was missing: the remaining uncovered I2C is the panel
driver's own expander/rail traffic (INT-driven PCA9535 polls, TPS power-good
checks) on its private mutex.

Fix (branch `i2c-sleep-hold`): wrap the Arduino HAL entry points
`i2cWrite`/`i2cRead`/`i2cWriteReadNonStop` with `busyAcquire`/`busyRelease`
(`src/i2c_bus.cpp`), so every TwoWire transaction — ours, the panel driver's,
any library's — holds light sleep off for its duration.


## 2026-10-03: one more on the sleep-hold build — and the real mechanism

Ride reset at 09:00:4x on build 7b74d630 (PR #96 wrappers in place). Dump
56b0afff, decoded with the exact ELF this time:

| core | where |
|---|---|
| 0 | ROM at 0x400559da under `_xt_lowint1` → `i2c_isr_handler_default` (i2c.c:553) → `esp_pm_impl_isr_hook`; current task **gps** (gps_service.cpp:642, draining the GPS UART) |
| 1 | IDLE in `esp_pm_impl_waiti`; the **epd_paint** task's stack sits in the same captured region: `EPD_Painter::_paint_task_body` → `TwoWire::requestFrom` → `__wrap_i2cWriteReadNonStop` → `i2c_master_write_read_device` → `i2c_master_cmd_begin` → `xQueueReceive` (i2c.c:1494) |

So the panel driver's register read was in flight **under the new sleep hold**,
and the last `pm window` before the reset says `calls=0 … holders=prlx+srlx+b1`:
no light sleep for the preceding minute. Sleep is not the trigger; the two
earlier dumps had the same paint-task chain on core 1 (their app addresses
line up with this build's minus the wrapper offset).

The actual hole is in IDF v4.4.6 `i2c_isr_handler_default`: when an enabled
interrupt bit is pending but `p_i2c->status` is neither WRITE nor READ (the
driver already moved it to TIMEOUT/ACK_ERROR/DONE on an earlier bit, or the
task-side timeout ran `i2c_hw_fsm_reset`, which on the S3 branch does not
mask interrupts), neither HAL event handler runs, `evt_type` stays ERR, and
the handler hits `// Do nothing if there is no proper event. return;` with
the interrupt still asserted. Level-triggered, it re-enters until the
interrupt watchdog fires. The enabled set is NACK/TIME_OUT/TRANS_COMPLETE/
ARBITRATION_LOST/END_DETECT, all "recognised", so the status mismatch is
the only way to reach that branch — which is why it needs a slow or
stretching slave (the TPS65185 during a rail transition) and the Arduino
50 ms transaction timeout.

Fix: `src/idf_overlay/i2c.c`, a copy of the v4.4.6 driver that clears and
masks the interrupt on that branch and counts it in `ot_i2c_isr_unhandled`;
`src/i2c_bus.cpp` logs each recovery. Linked from `src/` it supersedes the
archive member in `libdriver.a` (verified: `i2c_isr_handler_default` and
`i2c_master_cmd_begin` resolve to the overlay). The wrappers from the first
commit stay: they are still the right thing for the sleep case.
