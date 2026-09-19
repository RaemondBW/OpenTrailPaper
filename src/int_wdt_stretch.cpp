// Stretch the interrupt watchdog to INT_WDT_TIMEOUT_MS (config.h).
//
// IDF's interrupt watchdog fires when a core cannot service its tick for
// CONFIG_ESP_INT_WDT_TIMEOUT_MS (300 ms in the pinned framework). With light
// sleep on, core 1 stalls core 0 for the whole of esp_light_sleep_start()
// (esp_ipc_isr_stall_other_cpu, sleep_modes.c), and a sleep that runs long
// — a wait for the next BLE connection event with sensor links up — leaves
// core 0 silent past that budget. The stall and the watchdog are both level-4
// interrupts, so the watchdog fires the instant the stall lifts, at whatever
// instruction core 0 was interrupted on. Three rides' worth of coredumps
// (investigations/iwdt-light-sleep-2026-09-19.md) show exactly that: core 0
// frozen on a harmless branch in a level-1 ISR, core 1 already idle again.
// The core was not hung; the alarm was simply too tight for a sleeping build.
//
// The timeout is a Kconfig constant compiled into libesp_system, and its tick
// hook re-programs both watchdog stages from that constant on EVERY tick, so
// no runtime call can change it. What every one of those writes goes through
// is wdt_hal_config_stage(), a plain function in libhal — so the linker's
// --wrap (platformio.ini) routes them here and the ticks are scaled up. Only
// MWDT1 (the interrupt watchdog) is touched: the task watchdog is MWDT0 and
// the RTC watchdog used around boot and sleep is RWDT, and both pass through
// unchanged. Stage 1 (the hard reset at 2x) scales with stage 0.
//
// IRAM: this runs inside the tick ISR, including while the flash cache is off.

#include <stdint.h>
#include "esp_attr.h"
#include "sdkconfig.h"
#include "hal/wdt_hal.h"
#include "config.h"

static_assert(INT_WDT_TIMEOUT_MS >= CONFIG_ESP_INT_WDT_TIMEOUT_MS,
              "INT_WDT_TIMEOUT_MS only stretches the interrupt watchdog");

extern "C" void __real_wdt_hal_config_stage(wdt_hal_context_t* hal, wdt_stage_t stage,
                                            uint32_t timeout, wdt_stage_action_t behavior);

extern "C" void IRAM_ATTR __wrap_wdt_hal_config_stage(wdt_hal_context_t* hal, wdt_stage_t stage,
                                                      uint32_t timeout, wdt_stage_action_t behavior) {
    if (hal->inst == WDT_MWDT1 && (stage == WDT_STAGE0 || stage == WDT_STAGE1)) {
        // 32-bit is enough: the largest value IDF ever programs is the 5 s
        // boot-time stage (10,000 ticks), and 10,000 * INT_WDT_TIMEOUT_MS fits.
        timeout = timeout * (uint32_t)INT_WDT_TIMEOUT_MS / (uint32_t)CONFIG_ESP_INT_WDT_TIMEOUT_MS;
    }
    __real_wdt_hal_config_stage(hal, stage, timeout, behavior);
}
