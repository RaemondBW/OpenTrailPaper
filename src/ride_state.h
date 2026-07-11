#pragma once

#include <cstdint>
#include <ctime>

// Single shared snapshot of everything the UI and recorder consume.
// Producers (GPS task, BLE callbacks, battery poll) take the lock, update
// their fields, release. Consumers copy the whole struct under the lock.
//
// Plain data — also compiled on the host by tools/preview to render
// screen previews, so no Arduino dependencies in the struct itself.

struct RideState {
    // GPS
    bool     gpsFix = false;
    bool     everHadFix = false;   // lat/lon hold the last known position
    double   latitude = 0.0;
    double   longitude = 0.0;
    float    altitudeM = 0.0f;
    float    speedKmh = 0.0f;
    float    courseDeg = 0.0f;     // heading over ground, valid when moving
    uint8_t  satellites = 0;
    bool     timeValid = false;
    time_t   utc = 0;              // unix time from GPS

    // BLE sensors (0xFF/0xFFFF = no data, matching FIT invalid values)
    uint8_t  heartRateBpm = 0xFF;
    uint16_t powerW = 0xFFFF;      // instantaneous
    uint16_t power3sW = 0xFFFF;    // 3 s rolling average (hero display)
    uint8_t  cadenceRpm = 0xFF;
    bool     hrConnected = false;
    bool     powerConnected = false;
    bool     cadenceConnected = false;

    // Ride accumulation (owned by the recorder)
    bool     recording = false;
    double   distanceM = 0.0;
    uint32_t elapsedS = 0;
    uint32_t movingS = 0;
    float    gradePercent = 0.0f;
    bool     gradeValid = false;
    float    climbedM = 0.0f;

    // Battery
    uint8_t  batteryPercent = 0;
    bool     charging = false;

    // Rider config (mirrored from settings so renderers stay host-safe)
    uint16_t ftpW = 250;
    int16_t  tzMin = -420;
};

// End-of-ride stats for the summary screen (design 1g).
struct RideSummary {
    double   distanceM = 0.0;
    uint32_t movingS = 0;
    uint32_t elapsedS = 0;
    float    avgSpeedKmh = 0.0f;
    uint16_t avgPowerW = 0;        // 0 = no power data
    uint16_t normPowerW = 0;       // 0 = no power data
    uint8_t  avgHrBpm = 0;         // 0 = no HR data
    float    climbedM = 0.0f;
    time_t   startUtc = 0;
    time_t   endUtc = 0;
    int16_t  tzMin = -420;
};

#ifdef ARDUINO
#include <Arduino.h>

// Copy-out under lock; mutate under lock via with().
class SharedRideState {
public:
    void begin() { mutex_ = xSemaphoreCreateMutex(); }

    RideState snapshot() {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        RideState copy = state_;
        xSemaphoreGive(mutex_);
        return copy;
    }

    template <typename F>
    void with(F&& fn) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        fn(state_);
        xSemaphoreGive(mutex_);
    }

private:
    RideState state_;
    SemaphoreHandle_t mutex_ = nullptr;
};

extern SharedRideState g_state;
#endif
