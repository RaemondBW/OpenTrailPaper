#pragma once

// GPS on Serial2 (pins 43/44). Handles both module variants LilyGO ships:
// L76K (9600 baud, PCAS init strings) and u-blox MIA-M10Q (38400, UBX).
// Feeds TinyGPSPlus and publishes fixes into g_state.

#include <cstdint>

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

// FreeRTOS task: pumps NMEA into the parser and updates shared state.
void task(void* arg);

}
