#pragma once

// Speed and direction of travel from the companion phone's location stream.
//
// When the device's own receiver has no fix the phone's location is the
// position source (map, ride track). The phone streams a fix every 1-3 s
// (op 0x08 on the route characteristic, see ble_server). Newer apps append the
// phone's own speed and course; older ones send position only, and even a new
// app has no course while stationary (iOS reports course < 0, Android has no
// bearing). So:
//
//   speed  — the phone's, when it sent a valid one; otherwise derived from the
//            distance between successive fixes; 0 once the stream goes stale.
//   course — the phone's, when it sent one and the rider is moving; otherwise
//            the bearing between fixes far enough apart to mean something.
//            Stopped, the last course is HELD — never snapped back to north.
//
// The receiver path (gps_service) owns speedKmh/courseDeg whenever it has a
// fix of its own; publish() only writes them when it doesn't.
//
// Plain data + maths, no Arduino dependencies: host-tested by
// tools/phone_motion_test.

#include <cmath>
#include <cstdint>

#include "ride_state.h"

namespace phone_motion {

// The phone sends every 1 s while recording and every 3 s otherwise, and iOS
// can deliver late. Past this with nothing new, its speed is no longer current.
constexpr uint32_t kStaleMs = 7000;
// Only derive from fixes whose horizontal accuracy is at least this good —
// the same bar the recorder sets for putting a phone fix in the ride file. A
// cell/Wi-Fi grade fix wanders far enough to fake a walking pace.
constexpr float kMaxDeriveAccM = 50.0f;
// Minimum displacement before a derived speed is believed, and the longest
// window it is measured over. Below kMinMoveM in kMaxWindowMs, the rider is
// (near enough) stopped.
constexpr float kMinMoveM = 5.0f;
constexpr uint32_t kMaxWindowMs = 10000;
// Baseline for a course derived from positions: shorter than this and the
// fixes' own scatter dominates the bearing.
constexpr float kCourseBaselineM = 10.0f;
// A course (phone-reported or derived) only counts above this speed; below
// it, the direction of travel is noise.
constexpr float kCourseMinSpeedMs = 1.0f;
// Faster than this between fixes is a fix jumping, not a rider (144 km/h).
constexpr float kMaxSpeedMs = 40.0f;
// A gap this long means the stream restarted: re-anchor, claim nothing.
constexpr uint32_t kRestartGapMs = 30000;

inline double distanceM(double lat1, double lon1, double lat2, double lon2) {
    constexpr double R = 6371000.0, D2R = M_PI / 180.0;
    const double dLat = (lat2 - lat1) * D2R, dLon = (lon2 - lon1) * D2R;
    const double a = sin(dLat / 2) * sin(dLat / 2) +
                     cos(lat1 * D2R) * cos(lat2 * D2R) * sin(dLon / 2) * sin(dLon / 2);
    return 2 * R * atan2(sqrt(a), sqrt(1 - a));
}

// Initial bearing from 1 to 2, degrees clockwise from north, [0, 360).
inline float bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    constexpr double D2R = M_PI / 180.0;
    const double p1 = lat1 * D2R, p2 = lat2 * D2R, dl = (lon2 - lon1) * D2R;
    const double y = sin(dl) * cos(p2);
    const double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    double b = atan2(y, x) / D2R;
    if (b < 0) b += 360.0;
    if (b >= 360.0) b -= 360.0;
    return (float)b;
}

class Estimator {
public:
    // One phone fix. phoneSpeedMs / phoneCourseDeg are NAN when the phone did
    // not send them or flagged them invalid. accM <= 0 means "unknown".
    void update(double lat, double lon, float accM, uint32_t nowMs,
                float phoneSpeedMs, float phoneCourseDeg) {
        const bool goodFix = accM > 0 && accM <= kMaxDeriveAccM;
        if (haveAnchor_ && nowMs - anchorMs_ > kRestartGapMs) {
            haveAnchor_ = false;
            haveCourseAnchor_ = false;
            speedMs_ = 0.0f;
        }

        // --- speed ---
        float derived = NAN;
        if (!goodFix) {
            // Can't trust it for geometry; don't let it seed an anchor either.
            haveAnchor_ = false;
            haveCourseAnchor_ = false;
        } else if (!haveAnchor_) {
            anchor(lat, lon, nowMs);
        } else {
            const double d = distanceM(anchorLat_, anchorLon_, lat, lon);
            const uint32_t dtMs = nowMs - anchorMs_;
            const float dt = dtMs / 1000.0f;
            if (dt > 0.2f && d / dt > kMaxSpeedMs) {
                anchor(lat, lon, nowMs);           // jump: start again
                haveCourseAnchor_ = false;
            } else if (d >= kMinMoveM && dt > 0.2f) {
                derived = (float)(d / dt);
                anchor(lat, lon, nowMs);
            } else if (dtMs >= kMaxWindowMs) {
                derived = 0.0f;                    // < 0.5 m/s over the window
                anchor(lat, lon, nowMs);
            }
            // else: not enough baseline yet — keep the last value.
        }
        if (!std::isnan(phoneSpeedMs) && phoneSpeedMs >= 0.0f) {
            speedMs_ = phoneSpeedMs;
        } else if (!std::isnan(derived)) {
            speedMs_ = derived;
        } else if (!goodFix) {
            speedMs_ = 0.0f;                       // nothing to go on
        }
        lastFixMs_ = nowMs;
        haveFix_ = true;

        // --- course ---
        const bool moving = speedMs_ >= kCourseMinSpeedMs;
        if (moving && !std::isnan(phoneCourseDeg) && phoneCourseDeg >= 0.0f) {
            courseDeg_ = fmodf(phoneCourseDeg, 360.0f);
            courseValid_ = true;
        }
        if (goodFix) {
            if (!haveCourseAnchor_) {
                courseAnchor(lat, lon);
            } else if (distanceM(cLat_, cLon_, lat, lon) >= kCourseBaselineM) {
                // Only claim a derived course if the phone didn't give one.
                if (moving && (std::isnan(phoneCourseDeg) || phoneCourseDeg < 0.0f)) {
                    courseDeg_ = bearingDeg(cLat_, cLon_, lat, lon);
                    courseValid_ = true;
                }
                courseAnchor(lat, lon);
            }
            // While stopped, let the anchor follow so jitter accumulated over
            // a long stop isn't later read as a bearing.
            if (!moving) courseAnchor(lat, lon);
        }
    }

    // Speed in km/h, 0 once the stream has gone quiet.
    float speedKmh(uint32_t nowMs) const {
        if (!haveFix_ || nowMs - lastFixMs_ >= kStaleMs) return 0.0f;
        return speedMs_ * 3.6f;
    }
    bool courseValid() const { return courseValid_; }
    float courseDeg() const { return courseDeg_; }

private:
    void anchor(double lat, double lon, uint32_t ms) {
        anchorLat_ = lat; anchorLon_ = lon; anchorMs_ = ms; haveAnchor_ = true;
    }
    void courseAnchor(double lat, double lon) {
        cLat_ = lat; cLon_ = lon; haveCourseAnchor_ = true;
    }

    bool haveAnchor_ = false;
    double anchorLat_ = 0, anchorLon_ = 0;
    uint32_t anchorMs_ = 0;
    bool haveCourseAnchor_ = false;
    double cLat_ = 0, cLon_ = 0;
    bool haveFix_ = false;
    uint32_t lastFixMs_ = 0;
    float speedMs_ = 0.0f;
    bool courseValid_ = false;
    float courseDeg_ = 0.0f;
};

// Publish the phone-derived speed/course into the ride state when the phone
// is the position source, i.e. the device's own receiver has no fix. Call with
// the g_state lock held: from the BLE handler as each fix lands, and from the
// GPS task after it writes RideState (so a receiver without a fix can't
// overwrite the phone's speed with a latched or zeroed one) and on every loop
// (so the speed drops to 0 when the phone stream goes stale).
inline void publish(RideState& s, uint32_t nowMs) {
    if (s.gpsFix) return;   // the receiver owns speed/course while it has a fix
    const bool fresh = s.phoneFixValid && nowMs - s.phoneFixMs < kStaleMs;
    s.speedKmh = fresh ? s.phoneSpeedKmh : 0.0f;
    if (s.phoneCourseValid) s.courseDeg = s.phoneCourseDeg;   // held when stopped
}

}  // namespace phone_motion
