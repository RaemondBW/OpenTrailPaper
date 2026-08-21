#pragma once

#include <stddef.h>

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

// Configure automatic light sleep. Call once from setup(), AFTER the peripherals
// (especially SerialGPS) are begun, so the UART driver is installed. Returns
// true only if PM was actually enabled by the running framework.
bool begin();

// Call periodically from loop(): holds a no-light-sleep lock while the USB-CDC
// console is connected, so the serial monitor survives on the bench (light sleep
// drops the native-USB PHY and churns the CDC link). Released on battery so the
// CPU is free to sleep.
void tick();

// Hold light sleep off across a bus transaction that must not be interrupted.
//
// WHY: the SD card is driven by sd_diskio.cpp -> Arduino SPIClass ->
// esp32-hal-spi.c, which — unlike ESP-IDF's own spi_master — never takes a PM
// lock. With light sleep armed the SoC can therefore sleep in the middle of an
// SD command, gating the SPI clock and leaving the card mid-transaction. The
// card then refuses CMD0/GO_IDLE_STATE on every subsequent mount, reporting
// cardType=NONE, and stays that way until it is physically power-cycled —
// surviving reboots, reflashes and even a revert to a non-PM framework, which
// makes it look like a permanent hardware fault.
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

}  // namespace power_mgmt
