#pragma once

// GPS on Serial2 (pins 43/44). Handles both module variants LilyGO ships:
// L76K (9600 baud, PCAS init strings) and u-blox MIA-M10Q (38400, UBX).
// Feeds TinyGPSPlus and publishes fixes into g_state.

#include <cstdint>
#include <ctime>

// Raw receiver diagnostics for the GPS debug page.
struct GpsDebug {
    bool moduleDetected = false;
    uint32_t chars = 0;          // NMEA bytes seen
    uint32_t passedCksum = 0;
    uint32_t failedCksum = 0;
    uint32_t withFix = 0;        // sentences carrying a fix
    int satsInUse = 0;
    int satsInView = 0;
    int bestSnr = 0;      // strongest C/N0 heard (dB-Hz)
    float hdop = 0;
    bool locValid = false;
    uint32_t locAgeMs = 0;
    double lat = 0, lon = 0;
    float altM = 0, speedKmh = 0;
    int hour = -1, minute = 0, second = 0;  // UTC, -1 = no time yet
};

namespace gps_service {

void getDebug(GpsDebug& out);

// Powers the GPS rail (via IO expander, done in main) must happen first.
// Returns false if no module responded; the task is safe to start anyway.
bool begin();

// Detected chipset name for the GPS debug page ("CASIC", "u-blox", or "none").
const char* moduleName();

// AGPS warm-start seed: tell the receiver roughly where and when it is so it
// only searches the satellites actually overhead (cold -> warm start). Pass a
// last-known position; set haveTime only when `utc` is genuinely current (a
// stale time hurts more than it helps). Safe to call any time — used at boot
// with the saved NVS position and from the phone with a fresh CoreLocation fix.
void injectAiding(double lat, double lon, time_t utc, bool haveTime,
                  float posAccM, float timeAccS);

// Thread-safe way to hand the receiver a fresh position from the phone. Unlike
// injectAiding (which writes the GPS UART directly and must run on the GPS
// task), this just stashes the fix; the GPS task applies it on its next loop.
// Safe to call from the BLE callback.
void seedPosition(double lat, double lon, time_t utc, bool haveTime,
                  float posAccM);

// Testing/iteration: force a receiver COLD start (wipe ephemeris/almanac/time/
// position) so time-to-first-fix can be measured repeatedly over serial without
// a physical power-cycle. withAiding re-seeds position(+time) right after — the
// path we ship; withAiding=false measures the unaided baseline, to quantify how
// much the aiding actually helps. Handled on the GPS task; safe from any task.
void forceColdStart(bool withAiding);

// Detected chipset as a code for the BLE AGNSS query: 0 none, 1 CASIC, 2 u-blox.
int moduleKindCode();

// AGNSS (assisted GPS) ephemeris injection. The phone fetches a module-specific
// ephemeris blob and streams it here; we pipe the raw bytes to the receiver's
// UART (paced on the GPS task) — the module parses its own format. See
// design/agnss.md. All three are safe to call from the BLE task.
void agnssBegin();                             // start a session, re-seed AID-INI
void agnssInject(const uint8_t* data, size_t len);  // feed raw ephemeris bytes
void agnssEnd();                               // finish, re-seed AID-INI

// FreeRTOS task: pumps NMEA into the parser and updates shared state.
void task(void* arg);

}
