// Host tests for src/wheel_speed.h (CSC wheel data -> speed/distance) and
// src/speed_source.h (which speed the rider sees: sensor > GPS > phone > 0).

#include <cmath>
#include <cstdio>
#include <string>

#include "speed_source.h"
#include "wheel_speed.h"

namespace {

int g_fail = 0;
const char* g_case = "";

void check(bool ok, const std::string& what) {
    if (!ok) { printf("  FAIL  %s: %s\n", g_case, what.c_str()); ++g_fail; }
}
void checkNear(double got, double want, double tol, const std::string& what) {
    if (!(fabs(got - want) <= tol)) {
        printf("  FAIL  %s: %s (got %.3f, want %.3f +/- %.3f)\n",
               g_case, what.c_str(), got, want, tol);
        ++g_fail;
    }
}
void begin(const char* name) { g_case = name; }

constexpr uint16_t kCirc = 2105;
constexpr double kCircM = 2.105;
// km/h for `revs` wheel revolutions in `ticks` 1/1024 s at 2105 mm.
double kmh(double revs, double ticks) { return revs * kCircM / (ticks / 1024.0) * 3.6; }

// A steady rider: one sample per second, `revsPerSec` revolutions each.
struct Rider {
    wheel_speed::Estimator e;
    uint32_t revs = 1000;      // cumulative counter, mid-life
    uint16_t t = 5000;         // event time, 1/1024 s
    uint32_t now = 10000;      // device millis
    double dist = 0;
    float step(uint32_t dRevs, uint16_t dTicks = 1024, uint32_t dMs = 1000) {
        revs += dRevs;
        t = (uint16_t)(t + dTicks);
        now += dMs;
        const float d = e.update(revs, t, now, kCirc);
        dist += d;
        return d;
    }
    // Duplicate notification: nothing new, just time passing.
    float dup(uint32_t dMs = 1000) {
        now += dMs;
        const float d = e.update(revs, t, now, kCirc);
        dist += d;
        return d;
    }
};

void testParse() {
    begin("parse CSC measurement");
    wheel_speed::Measurement m;
    const uint8_t wheel[] = {0x01, 0x10, 0x27, 0x00, 0x00, 0x00, 0x04};   // 10000 revs, t=1024
    check(wheel_speed::parse(wheel, sizeof(wheel), m), "wheel-only parses");
    check(m.hasWheel && !m.hasCrank, "wheel-only flags");
    check(m.wheelRevs == 10000 && m.wheelTime == 1024, "wheel values");

    const uint8_t combo[] = {0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x80, 0x2A, 0x00, 0x00, 0x10};
    check(wheel_speed::parse(combo, sizeof(combo), m), "combo parses");
    check(m.hasWheel && m.hasCrank, "combo flags");
    check(m.wheelRevs == 0xFFFFFFFFu && m.wheelTime == 0x8000, "combo wheel (u32 max)");
    check(m.crankRevs == 42 && m.crankTime == 0x1000, "combo crank");

    const uint8_t crank[] = {0x02, 0x05, 0x00, 0x00, 0x04};
    check(wheel_speed::parse(crank, sizeof(crank), m), "crank-only parses");
    check(!m.hasWheel && m.hasCrank && m.crankRevs == 5, "crank-only values");

    const uint8_t shortWheel[] = {0x01, 0x10, 0x27, 0x00};
    check(!wheel_speed::parse(shortWheel, sizeof(shortWheel), m), "truncated wheel rejected");
    check(!wheel_speed::parse(wheel, 0, m), "empty rejected");
}

void testCircumferenceMath() {
    begin("speed = revs x circumference / event time");
    Rider r;
    r.step(0);                          // seed
    checkNear(r.dist, 0, 1e-9, "seed adds no distance");
    // 4 rev/s at 2105 mm = 8.42 m/s = 30.31 km/h.
    float d = r.step(4);
    checkNear(d, 4 * kCircM, 1e-4, "distance per sample");
    checkNear(r.e.speedKmh(r.now), kmh(4, 1024), 0.01, "30.3 km/h");
    checkNear(r.e.speedKmh(r.now), 30.31, 0.01, "30.3 km/h (literal)");
    // 3 revs in 0.75 s: same speed, odd interval.
    r.step(3, 768);
    checkNear(r.e.speedKmh(r.now), 30.31, 0.01, "3 revs / 768 ticks");

    // A different wheel: 1 rev/s on a 1590 mm 20" wheel = 5.72 km/h.
    wheel_speed::Estimator e;
    e.update(0, 0, 1000, 1590);
    e.update(1, 1024, 2000, 1590);
    checkNear(e.speedKmh(2000), 1.590 * 3.6, 0.01, "1590 mm wheel");
    // An invalid circumference falls back to the default rather than 0 km/h.
    wheel_speed::Estimator e2;
    e2.update(0, 0, 1000, 50);
    e2.update(1, 1024, 2000, 50);
    checkNear(e2.speedKmh(2000), kmh(1, 1024), 0.01, "invalid circumference -> default");
    check(!wheel_speed::validCircMm(50) && wheel_speed::validCircMm(2105) &&
              !wheel_speed::validCircMm(4000),
          "validCircMm range");
}

void testFirstSample() {
    begin("first sample only seeds");
    wheel_speed::Estimator e;
    // A sensor that has done 8,000 km: its cumulative counter is not a ride.
    float d = e.update(3800000, 31000, 5000, kCirc);
    checkNear(d, 0, 1e-9, "no distance from a cumulative value");
    checkNear(e.speedKmh(5000), 0, 1e-9, "no speed yet");
    check(e.fresh(5000), "fresh after the seed");
    d = e.update(3800003, 31000 + 1024, 6000, kCirc);
    checkNear(d, 3 * kCircM, 1e-4, "second sample counts");
    checkNear(e.speedKmh(6000), kmh(3, 1024), 0.01, "second sample speed");
    // reset() (reconnect) seeds again.
    e.reset();
    d = e.update(3900000, 100, 7000, kCirc);
    checkNear(d, 0, 1e-9, "after reset: seed again");
}

void testRevWrap() {
    begin("u32 revolution counter wraps");
    wheel_speed::Estimator e;
    e.update(0xFFFFFFFEu, 1000, 1000, kCirc);
    float d = e.update(0x00000002u, 2024, 2000, kCirc);   // 4 revs across the wrap
    checkNear(d, 4 * kCircM, 1e-4, "4 revs across wrap");
    checkNear(e.speedKmh(2000), kmh(4, 1024), 0.01, "speed across wrap");
    check(e.glitches() == 0, "not a glitch");
}

void testTimeWrap() {
    begin("u16 event time wraps every 64 s");
    wheel_speed::Estimator e;
    e.update(100, 65000, 1000, kCirc);
    float d = e.update(104, 500, 2000, kCirc);   // 65536-65000+500 = 1036 ticks
    checkNear(d, 4 * kCircM, 1e-4, "distance");
    checkNear(e.speedKmh(2000), kmh(4, 1036), 0.01, "speed across time wrap");
}

void testDuplicatesAndStopDecay() {
    begin("duplicates hold, then decay to 0 after kStopMs");
    Rider r;
    r.step(0);
    r.step(4);
    const float v = r.e.speedKmh(r.now);
    // Coasting: two duplicates 1 s apart — no new event, speed held.
    checkNear(r.dup(), 0, 1e-9, "duplicate adds nothing");
    checkNear(r.e.speedKmh(r.now), v, 1e-4, "held after 1 s");
    r.dup();
    checkNear(r.e.speedKmh(r.now), v, 1e-4, "held after 2 s");
    r.dup();   // 3 s since the last revolution
    checkNear(r.e.speedKmh(r.now), 0, 1e-9, "0 after 3 s with no revolution");
    check(r.e.fresh(r.now), "sensor still fresh (still talking)");
    check(!r.e.moving(r.now), "not moving");
    // Stopped for a long while, sensor still sending duplicates.
    for (int i = 0; i < 30; ++i) r.dup();
    checkNear(r.e.speedKmh(r.now), 0, 1e-9, "still 0");
    // Sensor goes silent: stale after kStaleMs.
    check(!r.e.fresh(r.now + wheel_speed::kStaleMs), "stale when silent");
}

void testRestartAfterStop() {
    begin("first revolution after a stop: distance, no bogus speed");
    Rider r;
    r.step(0);
    r.step(4);
    for (int i = 0; i < 20; ++i) r.dup();      // 20 s stop
    // One revolution whose event delta spans the stop (event 20 s later).
    const float d = r.step(1, 20 * 1024);
    checkNear(d, kCircM, 1e-4, "distance counted");
    checkNear(r.e.speedKmh(r.now), 0, 1e-9, "no 0.4 km/h average-over-the-stop");
    check(r.e.moving(r.now), "but it is moving (auto-pause evidence)");
    r.step(2, 1024);
    checkNear(r.e.speedKmh(r.now), kmh(2, 1024), 0.01, "next event gives the speed");
}

void testImplausibleJumps() {
    begin("implausible jumps are rejected and reseeded");
    Rider r;
    r.step(0);
    r.step(4);
    const double before = r.dist;
    // 500 revolutions in one second (3,788 km/h): a corrupted counter.
    float d = r.step(500);
    checkNear(d, 0, 1e-9, "no distance from the jump");
    check(r.e.glitches() == 1, "counted as a glitch");
    checkNear(r.dist, before, 1e-9, "total unchanged");
    // Normal riding resumes from the new baseline.
    d = r.step(4);
    checkNear(d, 4 * kCircM, 1e-4, "next sample counts");
    checkNear(r.e.speedKmh(r.now), kmh(4, 1024), 0.01, "speed back");

    // Revs advance a lot but the sensor's event time barely moved (a counter
    // jump the clock didn't share) — also rejected, by the wall-clock check.
    d = r.step(400, 30000, 1000);
    checkNear(d, 0, 1e-9, "wall-clock implausible");

    // Counter BACKWARDS (battery swap / sensor reset): reseed, no distance.
    wheel_speed::Estimator e;
    e.update(5000, 1000, 1000, kCirc);
    d = e.update(3, 2024, 2000, kCirc);
    checkNear(d, 0, 1e-9, "backwards counter adds nothing");
    d = e.update(7, 3048, 3000, kCirc);
    checkNear(d, 4 * kCircM, 1e-4, "counts from the reset value");

    // Revolutions with no event-time change: malformed, ignored until the
    // next good sample, which covers both.
    wheel_speed::Estimator e2;
    e2.update(100, 1000, 1000, kCirc);
    d = e2.update(102, 1000, 2000, kCirc);
    checkNear(d, 0, 1e-9, "revs without event time ignored");
    d = e2.update(104, 1000 + 2048, 3000, kCirc);
    checkNear(d, 4 * kCircM, 1e-4, "next good sample covers both");
}

void testLongGap() {
    begin("gap past the event clock's wrap: distance kept, speed not claimed");
    wheel_speed::Estimator e;
    e.update(100, 1000, 1000, kCirc);
    // 90 s of no packets (out of range), 300 revs = 631 m = 25 km/h on average.
    float d = e.update(400, 1234, 91000, kCirc);
    checkNear(d, 300 * kCircM, 1e-3, "plausible distance kept");
    checkNear(e.speedKmh(91000), 0, 1e-9, "no speed from an ambiguous delta");
    d = e.update(404, 1234 + 1024, 92000, kCirc);
    checkNear(e.speedKmh(92000), kmh(4, 1024), 0.01, "speed from the next sample");
    // An implausible one over the same kind of gap is dropped.
    d = e.update(404 + 100000, 9999, 182000, kCirc);
    checkNear(d, 0, 1e-9, "implausible over a long gap dropped");
}

void testDistanceAccumulation() {
    begin("distance accumulates over a ride");
    Rider r;
    r.step(0);
    for (int i = 0; i < 100; ++i) r.step(3, 1024);   // 300 revs
    checkNear(r.dist, 300 * kCircM, 1e-2, "631.5 m");
    // u32 wrap mid-ride doesn't lose or invent distance.
    Rider w;
    w.revs = 0xFFFFFFF0u;
    w.step(0);
    for (int i = 0; i < 10; ++i) w.step(3);   // crosses 0
    checkNear(w.dist, 30 * kCircM, 1e-3, "across the wrap");
}

// --- arbitration ----------------------------------------------------------

RideState sensorState(uint32_t now, float kmhVal) {
    RideState s;
    s.speedSensorConnected = true;
    s.wheelDataMs = now;
    s.wheelMoveMs = now;
    s.wheelSpeedKmh = kmhVal;
    return s;
}

void testArbitrationPriority() {
    begin("arbitration: sensor > GPS > phone > none");
    const uint32_t now = 100000;
    RideState s = sensorState(now, 31.0f);
    s.gpsFix = true;
    s.gpsSpeedKmh = 28.0f;
    s.phoneFixValid = true;
    s.phoneFixMs = now;
    s.phoneSpeedKmh = 25.0f;
    speed_source::publish(s, now);
    check(s.speedSource == speed_source::SENSOR, "sensor first");
    checkNear(s.speedKmh, 31.0, 1e-4, "sensor speed");

    s.speedSensorConnected = false;       // sensor gone
    speed_source::publish(s, now);
    check(s.speedSource == speed_source::GPS, "then GPS");
    checkNear(s.speedKmh, 28.0, 1e-4, "GPS speed");

    s.gpsFix = false;
    s.gpsSpeedKmh = 0;
    speed_source::publish(s, now);
    check(s.speedSource == speed_source::PHONE, "then phone");
    checkNear(s.speedKmh, 25.0, 1e-4, "phone speed");

    speed_source::publish(s, now + phone_motion::kStaleMs);
    check(s.speedSource == speed_source::NONE, "then nothing");
    checkNear(s.speedKmh, 0, 1e-9, "0");
}

void testSensorSurvivesGpsAndPhone() {
    begin("#99 stale-GPS zeroing and #103 phone publish don't clobber the sensor");
    const uint32_t now = 50000;
    RideState s = sensorState(now, 22.0f);
    s.gpsFix = true;
    s.gpsSpeedKmh = 21.0f;
    speed_source::publish(s, now);
    // gps_service's fix-expiry block: fix lost (tunnel).
    s.gpsFix = false;
    s.gpsSpeedKmh = 0.0f;
    speed_source::publish(s, now + 200);
    check(s.speedSource == speed_source::SENSOR, "still sensor after fix loss");
    checkNear(s.speedKmh, 22.0, 1e-4, "speed not zeroed");
    // Phone fix lands (ble_server) with a different speed.
    s.phoneFixValid = true;
    s.phoneFixMs = now + 300;
    s.phoneSpeedKmh = 9.0f;
    s.phoneCourseValid = true;
    s.phoneCourseDeg = 270.0f;
    speed_source::publish(s, now + 300);
    checkNear(s.speedKmh, 22.0, 1e-4, "phone doesn't override");
    checkNear(s.courseDeg, 270.0, 1e-4, "but phone still supplies heading");
}

void testSensorStopDecayInArbitration() {
    begin("sensor stopped -> 0 even with GPS wobble; sensor silent -> GPS");
    const uint32_t t0 = 20000;
    RideState s = sensorState(t0, 18.0f);
    s.gpsFix = true;
    s.gpsSpeedKmh = 2.5f;                 // stationary fix wobble
    // Sensor keeps talking but the wheel stopped at t0. Held below kStopMs...
    s.wheelDataMs = t0 + 2000;
    speed_source::publish(s, t0 + 2000);
    checkNear(s.speedKmh, 18.0, 1e-4, "held within kStopMs");
    // ...then 0.
    s.wheelDataMs = t0 + 4000;
    speed_source::publish(s, t0 + 4000);
    check(s.speedSource == speed_source::SENSOR, "sensor still the source");
    checkNear(s.speedKmh, 0, 1e-9, "decayed to 0, not GPS wobble");
    // Sensor goes silent (sleeps when stopped): falls back to GPS.
    speed_source::publish(s, t0 + 4000 + wheel_speed::kStaleMs);
    check(s.speedSource == speed_source::GPS, "silent sensor -> GPS");
    checkNear(s.speedKmh, 2.5, 1e-4, "GPS speed");
}

void testContradiction() {
    begin("wheel still while a good fix says 25 km/h -> GPS");
    const uint32_t t0 = 100000;
    RideState s = sensorState(t0, 0.0f);
    s.wheelMoveMs = 0;                     // magnet never seen
    s.gpsFix = true;
    s.gpsSpeedKmh = 25.0f;
    speed_source::publish(s, t0);
    check(s.speedSource == speed_source::GPS, "never-turned wheel loses to GPS");
    checkNear(s.speedKmh, 25.0, 1e-4, "GPS speed");
    // The wheel turns: sensor back in charge at once.
    s.wheelMoveMs = t0 + 1000;
    s.wheelDataMs = t0 + 1000;
    s.wheelSpeedKmh = 24.0f;
    speed_source::publish(s, t0 + 1000);
    check(s.speedSource == speed_source::SENSOR, "turning wheel wins");
    // Stopped for 5 s with GPS at 25: not yet contradicted (GPS lags a stop).
    s.wheelDataMs = t0 + 6000;
    speed_source::publish(s, t0 + 6000);
    check(s.speedSource == speed_source::SENSOR, "within kContradictMs: sensor");
    checkNear(s.speedKmh, 0, 1e-9, "0");
    s.wheelDataMs = t0 + 1000 + speed_source::kContradictMs;
    speed_source::publish(s, t0 + 1000 + speed_source::kContradictMs);
    check(s.speedSource == speed_source::GPS, "after kContradictMs: GPS");
    // Slow GPS (walking the bike) never contradicts.
    s.gpsSpeedKmh = 5.0f;
    speed_source::publish(s, t0 + 1000 + speed_source::kContradictMs);
    check(s.speedSource == speed_source::SENSOR, "slow GPS doesn't override");
}

}  // namespace

int main() {
    testParse();
    testCircumferenceMath();
    testFirstSample();
    testRevWrap();
    testTimeWrap();
    testDuplicatesAndStopDecay();
    testRestartAfterStop();
    testImplausibleJumps();
    testLongGap();
    testDistanceAccumulation();
    testArbitrationPriority();
    testSensorSurvivesGpsAndPhone();
    testSensorStopDecayInArbitration();
    testContradiction();
    if (g_fail) { printf("%d failure(s)\n", g_fail); return 1; }
    printf("wheel_speed: all tests passed\n");
    return 0;
}
