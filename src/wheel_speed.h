#pragma once

// Speed and distance from a Bluetooth Cycling Speed and Cadence sensor's wheel
// data (service 0x1816, CSC Measurement 0x2A5B):
//
//   flags u8          bit0 = wheel revolution data present
//                     bit1 = crank revolution data present
//   [wheel]  u32 cumulative wheel revolutions
//            u16 last wheel event time, 1/1024 s
//   [crank]  u16 cumulative crank revolutions
//            u16 last crank event time, 1/1024 s
//
// Speed is (revolutions since the last event) x circumference / (event-time
// delta). The sensor sends ~1 Hz whether or not the wheel turned, so most of
// the work is deciding what NOT to believe:
//
//   * first sample after (re)connect only seeds the counters — a cumulative
//     value is not a distance;
//   * both counters wrap (u32 revs after ~8.6M km, u16 event time every 64 s);
//     unsigned subtraction handles one wrap, and a gap longer than the event
//     clock's period makes the time delta ambiguous, so we re-seed;
//   * a notification with no new revolution repeats the last event — hold the
//     speed, then decay it to 0 once no new revolution has arrived for kStopMs
//     (coasting to a stop, or stopped with the sensor still talking);
//   * a counter that goes BACKWARDS (battery swap, sensor reset) or a delta
//     that would mean > kMaxSpeedKmh is a glitch: re-seed, claim nothing.
//
// Plain data + maths, no Arduino dependencies: host-tested by
// tools/wheel_speed_test.

#include <cstddef>
#include <cstdint>

namespace wheel_speed {

// 700x25c. Garmin/Wahoo's default for a road bike, and within ~3% of every
// common 700c/29er tyre, so an unconfigured sensor is roughly right.
constexpr uint16_t kDefaultCircMm = 2105;
// Accepted circumference range: a 12" kids' wheel (~940 mm) to a 29x3.0 plus
// tyre (~2380 mm) with margin either side. Anything outside is a typo.
constexpr uint16_t kMinCircMm = 800;
constexpr uint16_t kMaxCircMm = 3500;
inline bool validCircMm(int mm) { return mm >= kMinCircMm && mm <= kMaxCircMm; }

// No new wheel revolution for this long and the bike is stopped. At 2105 mm
// that is below 2.5 km/h — walking pace, where nobody reads the number.
constexpr uint32_t kStopMs = 3000;
// No notification at all for this long and the sensor is no longer a source
// (asleep, out of range). Sensors send ~1 Hz while awake.
constexpr uint32_t kStaleMs = 5000;
// Faster than this between two events is a counter glitch, not a rider.
constexpr float kMaxSpeedKmh = 120.0f;
// The event clock is u16 at 1/1024 s: it wraps every 64 s. A gap between
// samples longer than this (with margin) makes the time delta ambiguous.
constexpr uint32_t kMaxGapMs = 60000;

class Estimator {
public:
    // Forget everything — call on (re)connect. The next sample only seeds.
    void reset() { *this = Estimator(); }

    // One CSC wheel sample at device time nowMs. Returns the metres travelled
    // since the previous accepted sample (0 for the seed, a duplicate, or a
    // rejected glitch) for the caller's distance total.
    float update(uint32_t revs, uint16_t eventTime, uint32_t nowMs, uint16_t circMm) {
        if (!validCircMm(circMm)) circMm = kDefaultCircMm;
        const float circM = circMm / 1000.0f;
        if (!primed_) {
            seed(revs, eventTime, nowMs);
            return 0.0f;
        }
        const uint32_t gapMs = nowMs - lastSampleMs_;
        lastSampleMs_ = nowMs;

        const uint32_t dRevs = revs - revs_;          // wraps correctly
        if ((int32_t)dRevs < 0) {                     // counter went backwards
            seed(revs, eventTime, nowMs);
            ++glitches_;
            return 0.0f;
        }
        if (dRevs == 0) return 0.0f;                  // no new event: hold, decay

        const float distM = dRevs * circM;
        if (gapMs > kMaxGapMs) {
            // Event-time delta is ambiguous across >1 wrap. Keep the distance
            // if wall-clock time says it is plausible, but claim no speed.
            const float wallKmh = distM / (gapMs / 1000.0f) * 3.6f;
            seed(revs, eventTime, nowMs);
            if (wallKmh > kMaxSpeedKmh) { ++glitches_; return 0.0f; }
            lastMoveMs_ = nowMs;
            speedKmh_ = 0.0f;
            return distM;
        }

        const uint16_t dTicks = (uint16_t)(eventTime - eventTime_);   // wraps
        if (dTicks == 0) {
            // Revolutions without an event time: malformed. Leave the stored
            // state alone so the next good sample covers both.
            return 0.0f;
        }
        const float dt = dTicks / 1024.0f;
        const float kmh = distM / dt * 3.6f;
        // Plausibility, judged on BOTH clocks: the sensor's (a tiny dTicks
        // with revs advancing) and ours (more revolutions than the wall-clock
        // gap allows — a counter jump the event time didn't share).
        const float wallSec = gapMs / 1000.0f + 1.0f;   // sample jitter slack
        const float wallKmh = distM / wallSec * 3.6f;
        if (kmh > kMaxSpeedKmh || wallKmh > kMaxSpeedKmh) {
            seed(revs, eventTime, nowMs);
            ++glitches_;
            return 0.0f;
        }
        revs_ = revs;
        eventTime_ = eventTime;
        // The first revolution after a stop spans the whole stop: its average
        // is meaningless (1 rev / 40 s = 0.2 km/h). Count the distance, and
        // let the next event give the speed.
        const bool wasStopped = lastMoveMs_ == 0 || nowMs - lastMoveMs_ >= kStopMs;
        lastMoveMs_ = nowMs;
        speedKmh_ = (wasStopped && dTicks > kStopMs * 1024 / 1000) ? 0.0f : kmh;
        return distM;
    }

    // Current speed: held between events, 0 after kStopMs without a new
    // revolution, 0 once the sensor has gone quiet.
    float speedKmh(uint32_t nowMs) const {
        if (!fresh(nowMs) || !moving(nowMs)) return 0.0f;
        return speedKmh_;
    }
    // A sample has arrived recently enough for the sensor to count as a source.
    bool fresh(uint32_t nowMs) const {
        return primed_ && nowMs - lastSampleMs_ < kStaleMs;
    }
    bool moving(uint32_t nowMs) const {
        return lastMoveMs_ != 0 && nowMs - lastMoveMs_ < kStopMs;
    }
    uint32_t lastMoveMs() const { return lastMoveMs_; }
    uint32_t lastSampleMs() const { return lastSampleMs_; }
    uint32_t glitches() const { return glitches_; }

private:
    void seed(uint32_t revs, uint16_t eventTime, uint32_t nowMs) {
        revs_ = revs;
        eventTime_ = eventTime;
        lastSampleMs_ = nowMs;
        primed_ = true;
    }

    bool primed_ = false;
    uint32_t revs_ = 0;
    uint16_t eventTime_ = 0;
    uint32_t lastSampleMs_ = 0;
    uint32_t lastMoveMs_ = 0;
    float speedKmh_ = 0.0f;
    uint32_t glitches_ = 0;
};

// CSC Measurement payload, decoded. Returns false for a malformed packet.
struct Measurement {
    bool hasWheel = false;
    uint32_t wheelRevs = 0;
    uint16_t wheelTime = 0;
    bool hasCrank = false;
    uint16_t crankRevs = 0;
    uint16_t crankTime = 0;
};

inline bool parse(const uint8_t* d, size_t len, Measurement& m) {
    m = Measurement();
    if (len < 1) return false;
    const uint8_t flags = d[0];
    size_t off = 1;
    if (flags & 0x01) {
        if (len < off + 6) return false;
        m.hasWheel = true;
        m.wheelRevs = (uint32_t)d[off] | ((uint32_t)d[off + 1] << 8) |
                      ((uint32_t)d[off + 2] << 16) | ((uint32_t)d[off + 3] << 24);
        m.wheelTime = (uint16_t)(d[off + 4] | (d[off + 5] << 8));
        off += 6;
    }
    if (flags & 0x02) {
        if (len < off + 4) return false;
        m.hasCrank = true;
        m.crankRevs = (uint16_t)(d[off] | (d[off + 1] << 8));
        m.crankTime = (uint16_t)(d[off + 2] | (d[off + 3] << 8));
    }
    return true;
}

// CSC Feature (0x2A5C) bits.
constexpr uint16_t kFeatureWheel = 0x0001;
constexpr uint16_t kFeatureCrank = 0x0002;

}  // namespace wheel_speed
