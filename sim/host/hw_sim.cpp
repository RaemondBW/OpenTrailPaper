// Stand-ins for the board's hardware modules. Each keeps the module's
// interface and reports the hardware as the sim has it: no
// Qwiic sensors, no RTC chip, no USB stack (a host mounting the card is the
// control channel's `usb`); power management is inert. The crash reporter is
// the firmware's own (src/crash_report.cpp). The fuel gauge's reading is set by the control
// channel (`battery`).
#include <Arduino.h>

#include "aux_sensors.h"
#include "board_power.h"
#include "crash_report.h"
#include "diag.h"
#include "gps_rx_guard.h"
#include "lora_radio.h"
#include "power_mgmt.h"
#include "ride_state.h"
#include "rtc_clock.h"
#include "sd_diagnostics.h"
#include "smooth_epd.h"
#include "usb_storage.h"
#include "sim.h"

#include <atomic>
#include <map>
#include <mutex>

extern SharedRideState g_state;

namespace power_mgmt {
bool prepare() { return true; }
bool begin() { return true; }
void requestSleep(bool) {}
void report() { Serial.println("[pm] sim: no power management"); }
void tick() {}
void busyAcquire() {}
void busyRelease() {}
bool busyReady() { return true; }
void frontlightHold(bool) {}
void stateStr(char* out, size_t n) { snprintf(out, n, "sim"); }
void sleepStats(uint32_t& ok, uint32_t& rejected, uint64_t& us) { ok = rejected = 0; us = 0; }
void sleepStatsMax(uint32_t& maxMs, uint32_t& longCallsTotal) { maxMs = longCallsTotal = 0; }
}  // namespace power_mgmt

namespace {
std::atomic<bool> g_usbHost{false};
}
void sim::setUsbHost(bool on) { g_usbHost = on; }
bool sim::usbHost() { return g_usbHost; }

// No USB stack: a test plays the computer (control `usb`), editing <dir>/sd
// while the host "has" it.
namespace usb_storage {
void begin() {}
void setDriveEnabled(bool) {}
bool driveEnabled() { return g_usbHost; }
bool hostActive() { return g_usbHost; }
void poll() {}
}  // namespace usb_storage

namespace sd_diagnostics {
void start(unsigned) { Serial.println("[sd] sim: no SD bus diagnostics"); }
void status() {}
void tick() {}
}  // namespace sd_diagnostics

namespace rtc_clock {
bool begin() { return false; }
bool read(time_t&) { return false; }
void write(time_t) {}
}  // namespace rtc_clock

namespace aux_sensors {
bool begin() { return false; }
void task(void*) { for (;;) vTaskDelay(60000); }
bool haveBaro() { return false; }
bool haveCompass() { return false; }
bool haveMotion() { return false; }
uint8_t baroAddress() { return 0; }
float seaLevelPa() { return 101325.0f; }
bool seaLevelCalibrated() { return false; }
void report(char* out, size_t n) { snprintf(out, n, "aux: none (sim)"); }
}  // namespace aux_sensors

// lora_radio: sim/host/lora_sim.cpp (fitted with --lora).

namespace smooth_epd {
void selfTest() {}
}

namespace gps_rx_guard {
void begin() {}
void tick(bool) {}
bool beforeSleep() { return true; }
void afterSleep() {}
void report() {}
uint32_t waitMs() { return 50; }
}  // namespace gps_rx_guard

void diag::drainDriverLogs() {}

// ---- board ------------------------------------------------------------------------

namespace {
std::atomic<int> g_batteryPct{100};
std::atomic<bool> g_charging{false};
}

void sim::setBattery(int pct, bool charging) {
    g_batteryPct = pct;
    g_charging = charging;
    g_state.with([&](RideState& s) {
        s.batteryPercent = (uint8_t)pct;
        s.charging = charging;
    });
}

void board_radio_power(bool) {}
bool board_side_button_pressed() { return sim::buttonDown("side"); }
bool board_read_power(uint16_t& mv, int16_t& ma) {
    mv = (uint16_t)(3500 + g_batteryPct.load() * 7);
    ma = g_charging ? 400 : -60;
    return true;
}
bool board_gauge_report(char* out, size_t n) {
    snprintf(out, n, "soc %d%% (sim) %s", g_batteryPct.load(), g_charging ? "charging" : "discharging");
    return true;
}
bool board_usb_power_present(bool& present) {
    present = g_charging.load();
    return true;
}

// ---- PWM outputs (cov/workouts) ---------------------------------------------------------
// analogWrite() as the LEDC would take it: the duty is kept per pin, so a test
// can read what the frontlight is actually driven at (status "backlight_pwm").
namespace {
std::mutex g_pwmM;
std::map<int, int> g_pwm;
}
void analogWrite(int pin, int duty) {
    std::lock_guard<std::mutex> lk(g_pwmM);
    g_pwm[pin] = duty;
}
int sim::pinDuty(int pin) {
    std::lock_guard<std::mutex> lk(g_pwmM);
    auto it = g_pwm.find(pin);
    return it == g_pwm.end() ? -1 : it->second;
}
