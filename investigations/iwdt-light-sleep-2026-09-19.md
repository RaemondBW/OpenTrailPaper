
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
