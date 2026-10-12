#pragma once

// Which speed the rider sees: one arbitration, used by every writer.
//
//   1. SENSOR — a wheel speed sensor that is talking (wheel_speed.h). Works
//      indoors, on a trainer and in tunnels, and answers within a revolution
//      where GPS speed lags by seconds. Its speed decays to 0 kStopMs after
//      the last revolution.
//   2. GPS    — the device's own receiver, while it has a fresh fix (#99).
//   3. PHONE  — the companion phone's stream, while it is fresh (#103).
//   4. NONE   — 0.
//
// One exception to 1: a sensor that says "stopped" while a good fix says the
// bike is clearly rolling has lost its magnet, been fitted to the wrong
// wheel, or is reporting for a bike that is not this one. After
// kContradictMs of that, GPS wins until the wheel turns again.
//
// Every producer writes ITS OWN field (gpsSpeedKmh, phoneSpeedKmh,
// wheelSpeedKmh/wheelMoveMs/wheelDataMs) and then calls publish() under the
// g_state lock, so whichever task wrote last, speedKmh is the same answer —
// the stale-GPS zeroing in gps_service and the phone path can no longer
// overwrite a fresh sensor speed, and a sensor going quiet falls straight back
// to GPS/phone on the next publish. Callers: gps_service (each fix, fix expiry
// and every loop), ble_server (each phone fix), ble_sensors (each CSC packet
// and its 1 Hz task loop — which is what decays a stopped wheel to 0).
//
// An ANT+ speed sensor (or any future wheel source) plugs in by writing the
// same wheel* fields; nothing here cares which radio they came from.
//
// Plain data, no Arduino dependencies: host-tested by tools/wheel_speed_test.

#include <cstdint>

#include "phone_motion.h"
#include "ride_state.h"
#include "wheel_speed.h"

namespace speed_source {

enum Source : uint8_t { NONE = 0, SENSOR = 1, GPS = 2, PHONE = 3 };

inline const char* name(uint8_t s) {
    switch (s) {
        case SENSOR: return "sensor";
        case GPS:    return "gps";
        case PHONE:  return "phone";
        default:     return "none";
    }
}

// GPS this fast with the wheel still for kContradictMs: believe GPS.
constexpr float kContradictKmh = 15.0f;
constexpr uint32_t kContradictMs = 10000;

// The wheel sensor is alive (packets arriving), whatever it says.
inline bool sensorFresh(const RideState& s, uint32_t nowMs) {
    return s.speedSensorConnected && s.wheelDataMs != 0 &&
           nowMs - s.wheelDataMs < wheel_speed::kStaleMs;
}

// The sensor's speed now: held between revolutions, 0 once none has arrived
// for kStopMs.
inline float sensorSpeedKmh(const RideState& s, uint32_t nowMs) {
    if (!sensorFresh(s, nowMs)) return 0.0f;
    if (s.wheelMoveMs == 0 || nowMs - s.wheelMoveMs >= wheel_speed::kStopMs) return 0.0f;
    return s.wheelSpeedKmh;
}

inline bool phoneFresh(const RideState& s, uint32_t nowMs) {
    return s.phoneFixValid && nowMs - s.phoneFixMs < phone_motion::kStaleMs;
}

inline Source pick(const RideState& s, uint32_t nowMs) {
    if (sensorFresh(s, nowMs)) {
        const bool wheelStill =
            s.wheelMoveMs == 0 || nowMs - s.wheelMoveMs >= kContradictMs;
        const bool contradicted =
            s.gpsFix && s.gpsSpeedKmh > kContradictKmh && wheelStill;
        if (!contradicted) return SENSOR;
    }
    if (s.gpsFix) return GPS;
    if (phoneFresh(s, nowMs)) return PHONE;
    return NONE;
}

// Recompute speedKmh/speedSource (and the phone's heading when the receiver
// has no fix). Call with the g_state lock held.
inline void publish(RideState& s, uint32_t nowMs) {
    const Source src = pick(s, nowMs);
    switch (src) {
        case SENSOR: s.speedKmh = sensorSpeedKmh(s, nowMs); break;
        case GPS:    s.speedKmh = s.gpsSpeedKmh; break;
        case PHONE:  s.speedKmh = s.phoneSpeedKmh; break;
        default:     s.speedKmh = 0.0f; break;
    }
    s.speedSource = src;
    // Heading is not speed: the receiver owns it while it has a fix, the phone
    // (held when stopped) otherwise. A wheel sensor has no direction.
    if (!s.gpsFix && s.phoneCourseValid) s.courseDeg = s.phoneCourseDeg;
}

}  // namespace speed_source
