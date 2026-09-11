#pragma once

#include <stddef.h>
#include <stdint.h>

// Automatic light-sleep power management for the ESP32-S3.
//
// IMPORTANT: this only DOES anything on a framework built with CONFIG_PM_ENABLE
// and CONFIG_FREERTOS_USE_TICKLESS_IDLE (see
// investigations/archive/cpu-sleep-spike.md). The stock precompiled Arduino
// framework compiles those OUT, so esp_pm_configure()
// returns ESP_ERR_NOT_SUPPORTED and this degrades to a logged no-op — the code
// is safe to build (and even flash): it simply won't sleep until the framework
// is rebuilt with power management enabled.
namespace power_mgmt {

// Create guards before peripheral/background task initialization.
bool prepare();
// Configure automatic light sleep after peripherals have started.
// Returns false on a stock framework or any guard/configuration failure.
bool begin();
void requestSleep(bool enabled); // applied by tick(); on/off A/B without reflashing
void report(); // configuration and IDF driver locks to Serial + buffered SD log

// Call periodically from loop(): holds a no-light-sleep lock while the USB-CDC
// console or VBUS is present, so the serial monitor survives on the bench.
// Released on battery once BLE and bus guards allow sleeping.
void tick();

// Hold light sleep off across a bus transaction that must not be interrupted.
//
// Arduino SPI does not acquire IDF PM locks. Guard the whole SD operation,
// including retries/delays, not just individual clock bursts. A protocol fault
// is possible without this guard; it does not prove a permanently wedged card.
//
// Recursive-safe: esp_pm_lock keeps a count, so nested acquire/release pairs
// balance. Both are no-ops when PM is unavailable (stock framework).
void busyAcquire();
void busyRelease();

// One-line summary of everything currently holding light sleep off — "grace",
// "serial", "phone", "hunt", "busy=N" — or "clear" when the CPU is free to
// sleep. Written onto every battery log line so a drain regression names its
// suspect in the same sample that shows the current, instead of needing a
// day of log correlation to find (which is how the sensor-hunt regression
// hid for a week). Never contains '%' — the phone's battery-line parser
// keys on that character.
void stateStr(char* out, size_t n);

// Successful light-sleep calls and time inside them (includes entry/exit overhead).
void sleepStats(uint32_t& ok, uint32_t& rejected, uint64_t& us);

}  // namespace power_mgmt
