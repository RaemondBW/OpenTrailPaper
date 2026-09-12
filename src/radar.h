#pragma once
#include <stddef.h>
#include <stdint.h>

// Host-safe Varia legacy BLE data model. Distance is reported by the radar;
// the third target byte has inconsistent semantics across models, so we do
// not invent speed, bearing or time-to-collision from it.
constexpr int RADAR_MAX_TARGETS = 8;
constexpr uint32_t RADAR_TARGET_TIMEOUT_MS = 2000;
constexpr uint32_t RADAR_SIGNAL_TIMEOUT_MS = 2000;
struct RadarTarget {
    uint8_t id = 0;
    uint8_t distanceM = 0;
    uint32_t seenMs = 0;
};
struct RadarState {
    bool connected = false;
    bool received = false;
    bool live = false;
    uint32_t packetMs = 0;
    uint8_t sequence = 0;
    uint8_t count = 0;
    RadarTarget targets[RADAR_MAX_TARGETS];
};
// Malformed/unknown packets leave state untouched and cannot refresh liveness.
// Heartbeats keep the link live; individual tracks expire independently so a
// fragmented notification cannot erase the other half of the vehicle set.
bool radarIngest(RadarState& state, const uint8_t* data, size_t len, uint32_t now);
void radarExpire(RadarState& state, uint32_t now);
