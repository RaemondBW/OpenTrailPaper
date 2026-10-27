// Stand-ins for the board's hardware modules. Each keeps the module's
// interface and reports the hardware as the sim has it: no LoRa radio, no
// Qwiic sensors, no RTC chip, no USB stack (a host mounting the card is the
// control channel's `usb`); power management and the
// crash reporter are inert. The fuel gauge's reading is set by the control
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

namespace lora_radio {
bool begin(const Config&) { return false; }
bool ok() { return false; }
float frequencyMHz() { return 0; }
bool irqFired() { return false; }
bool transmitting() { return false; }
size_t read(uint8_t*, size_t, float&, float&) { return 0; }
bool startSend(const uint8_t*, size_t) { return false; }
void finishSend() {}
void abortSend() {}
void listen() {}
bool channelBusy() { return false; }
void sleep() {}
float instantRssi() { return -120; }
uint32_t airtimeMs(size_t len) { return (uint32_t)len * 8; }
int lastError() { return -2; }
}  // namespace lora_radio

namespace smooth_epd {
void selfTest() {}
}

namespace crash_report {
void begin(int, const char*, const char*) {}
void tick() {}
void onSdMounted(const char*) {}
void recordLine(const char*, size_t) {}
void requestTestPanic() { Serial.println("[crash] sim: test panic ignored"); }
void requestStatus() { Serial.println("[crash] sim: no crash records"); }
}  // namespace crash_report

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
