#pragma once

// Automatic light-sleep power management for the ESP32-S3.
//
// IMPORTANT: this only DOES anything on a framework built with CONFIG_PM_ENABLE
// and CONFIG_FREERTOS_USE_TICKLESS_IDLE (see CPU_SLEEP_SPIKE.md). The stock
// precompiled Arduino framework compiles those OUT, so esp_pm_configure()
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

}  // namespace power_mgmt
