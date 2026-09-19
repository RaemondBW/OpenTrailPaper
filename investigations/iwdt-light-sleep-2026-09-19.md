# Interrupt-watchdog resets on the v1.19 light-sleep build (2026-09-19 ride)

Decoded from the card's `crash-*.log` + `memfault-*.log` with the matching ELF
(`.worktrees/main-battery-default/.pio/memfault-symbols/84c54018…/d882f257….elf`,
build Sep 12 2026 23:31:22, sha matches the crash report).

Three interrupt-watchdog resets on this build, all the same shape:

| when | core 0 (current task `gps`) | core 1 | light sleep just before |
|---|---|---|---|
| 09-13 12:12 | `i2c_isr_handler_default` i2c.c:491 (`addi.n a11,a1,4`) | `esp_pm_impl_waiti` (IDLE) | no — `holders=vbus+prlx calls=0` (on USB) |
| 09-13 16:11 | `i2c_isr_handler_default` i2c.c:510 (`beqi`) | `esp_pm_impl_waiti` (IDLE) | yes — 1400 calls, 23 s/min; "sensor sleep: armed" 12 s earlier |
| 09-19 08:14 | `esp_pm_impl_isr_hook` → inlined `leave_idle` pm_impl.c:567 (`beqz`) | `esp_pm_impl_waiti` (IDLE) | yes — 955 calls, 18 s/min; "sensor sleep: armed" 10 s earlier, HR+power links up |

Core 0 is always frozen on a plain register instruction inside a level-1
interrupt handler — nothing that can hang on its own — and core 1 is always
back in idle. That is what a core looks like after `esp_light_sleep_start()`
(sleep_modes.c:732, IDF v4.4.6) has stalled it via `esp_ipc_isr_stall_other_cpu`
for longer than `CONFIG_ESP_INT_WDT_TIMEOUT_MS=300`: the stall and the
interrupt watchdog are both level-4 interrupts, so the WDT fires the instant the
stall lifts, at the interrupted instruction, with core 1 already idle again.

The 12:12 case had light sleep held off by USB, so it needs another stall source
(flash erase/write also stalls the other core) or another cause; not pinned.

Not a panic in our code: no application frame is on either core. The feature in
play is established-link sleep (#74, on by default since #76; console
`sensorsleep off` disables it for one boot). Same build also logged
"sensor sleep fallback: HR supervision timeout after sleep" at 08:48.
