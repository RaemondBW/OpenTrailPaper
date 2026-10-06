// Host tests for src/phone_motion.h — speed and heading from the companion
// phone's location stream when the device's own GPS has no fix.

#include <cstdio>
#include <cmath>
#include <string>

#include "phone_motion.h"

namespace {

int g_fail = 0;
const char* g_case = "";

void check(bool ok, const std::string& what) {
    if (!ok) { printf("  FAIL  %s: %s\n", g_case, what.c_str()); ++g_fail; }
}
void checkNear(float got, float want, float tol, const std::string& what) {
    if (!(fabsf(got - want) <= tol)) {
        printf("  FAIL  %s: %s (got %.2f, want %.2f +/- %.2f)\n",
               g_case, what.c_str(), got, want, tol);
        ++g_fail;
    }
}
void begin(const char* name) { g_case = name; }

constexpr double kLat0 = 47.0, kLon0 = 8.0;
constexpr double kMPerDegLat = 111195.0;
const double kMPerDegLon = 111195.0 * cos(kLat0 * M_PI / 180.0);

// Position after travelling `m` metres on bearing `deg` from the origin.
void offset(double m, float deg, double& lat, double& lon) {
    const double r = deg * M_PI / 180.0;
    lat = kLat0 + m * cos(r) / kMPerDegLat;
    lon = kLon0 + m * sin(r) / kMPerDegLon;
}

void testPhoneValuesUsed() {
    begin("phone speed/course used when sent");
    phone_motion::Estimator e;
    double lat, lon;
    for (int i = 0; i < 5; ++i) {
        offset(i * 6.0, 90, lat, lon);
        e.update(lat, lon, 5, 1000 + i * 1000, 6.0f, 93.0f);
    }
    checkNear(e.speedKmh(5000), 21.6f, 0.01f, "speed km/h");
    check(e.courseValid(), "course valid");
    checkNear(e.courseDeg(), 93.0f, 0.01f, "course");
}

void testDerivedWhenAbsent() {
    begin("old app: derived from positions");
    phone_motion::Estimator e;
    double lat, lon;
    // 5 m/s south-west, 1 s apart.
    for (int i = 0; i < 6; ++i) {
        offset(i * 5.0, 225, lat, lon);
        e.update(lat, lon, 8, 1000 + i * 1000, NAN, NAN);
    }
    checkNear(e.speedKmh(6000), 18.0f, 0.5f, "speed km/h");
    check(e.courseValid(), "course valid");
    checkNear(e.courseDeg(), 225.0f, 1.0f, "course");
}

void testIosInvalidCourseStopped() {
    begin("stopped (iOS course -1): heading held, speed 0");
    phone_motion::Estimator e;
    double lat, lon;
    for (int i = 0; i < 5; ++i) {
        offset(i * 5.0, 30, lat, lon);
        e.update(lat, lon, 5, 1000 + i * 1000, 5.0f, 30.0f);
    }
    // Stop: phone speed 0, no course; positions jitter a couple of metres.
    double sLat, sLon;
    offset(20.0, 30, sLat, sLon);
    for (int i = 0; i < 20; ++i) {
        double jLat = sLat + ((i % 3) - 1) * 2.0 / kMPerDegLat;
        double jLon = sLon + ((i % 2) ? 2.0 : -2.0) / kMPerDegLon;
        e.update(jLat, jLon, 5, 6000 + i * 1000, 0.0f, NAN);
    }
    checkNear(e.speedKmh(25000), 0.0f, 0.001f, "speed");
    check(e.courseValid(), "course still valid");
    checkNear(e.courseDeg(), 30.0f, 0.01f, "course held, not north");
}

void testDerivedStopped() {
    begin("old app stopped: derived speed decays to 0");
    phone_motion::Estimator e;
    double lat, lon;
    for (int i = 0; i < 5; ++i) {
        offset(i * 5.0, 180, lat, lon);
        e.update(lat, lon, 5, 1000 + i * 1000, NAN, NAN);
    }
    for (int i = 0; i < 12; ++i)
        e.update(lat, lon, 5, 6000 + i * 1000, NAN, NAN);
    checkNear(e.speedKmh(17000), 0.0f, 0.001f, "speed");
    checkNear(e.courseDeg(), 180.0f, 1.0f, "course held");
}

void testStale() {
    begin("stream stale -> 0");
    phone_motion::Estimator e;
    e.update(kLat0, kLon0, 5, 1000, 8.0f, 10.0f);
    check(e.speedKmh(2000) > 28.0f, "fresh speed");
    checkNear(e.speedKmh(1000 + phone_motion::kStaleMs), 0.0f, 0.001f, "stale speed");
}

void testPoorAccuracy() {
    begin("cell-grade fix: no derived speed");
    phone_motion::Estimator e;
    double lat, lon;
    for (int i = 0; i < 5; ++i) {
        offset(i * 30.0, 0, lat, lon);   // wandering network-grade fixes
        e.update(lat, lon, 200, 1000 + i * 1000, NAN, NAN);
    }
    checkNear(e.speedKmh(5000), 0.0f, 0.001f, "speed");
    check(!e.courseValid(), "no course");
}

void testJumpRejected() {
    begin("fix jump: no speed claim");
    phone_motion::Estimator e;
    double lat, lon;
    e.update(kLat0, kLon0, 5, 1000, NAN, NAN);
    offset(500.0, 0, lat, lon);
    e.update(lat, lon, 5, 2000, NAN, NAN);
    checkNear(e.speedKmh(2000), 0.0f, 0.001f, "speed");
}

void testPublish() {
    begin("publish: receiver fix wins; phone otherwise; stale -> 0");
    RideState s;
    s.phoneFixValid = true;
    s.phoneFixMs = 1000;
    s.phoneSpeedKmh = 20.0f;
    s.phoneCourseValid = true;
    s.phoneCourseDeg = 135.0f;
    s.gpsFix = true;
    s.speedKmh = 30.0f;
    s.courseDeg = 10.0f;
    phone_motion::publish(s, 2000);
    checkNear(s.speedKmh, 30.0f, 0.001f, "receiver speed kept");
    checkNear(s.courseDeg, 10.0f, 0.001f, "receiver course kept");
    s.gpsFix = false;
    phone_motion::publish(s, 2000);
    checkNear(s.speedKmh, 20.0f, 0.001f, "phone speed");
    checkNear(s.courseDeg, 135.0f, 0.001f, "phone course");
    phone_motion::publish(s, 1000 + phone_motion::kStaleMs);
    checkNear(s.speedKmh, 0.0f, 0.001f, "stale speed");
    checkNear(s.courseDeg, 135.0f, 0.001f, "course held");
}

}  // namespace

int main() {
    testPhoneValuesUsed();
    testDerivedWhenAbsent();
    testIosInvalidCourseStopped();
    testDerivedStopped();
    testStale();
    testPoorAccuracy();
    testJumpRejected();
    testPublish();
    if (g_fail) { printf("%d failure(s)\n", g_fail); return 1; }
    printf("phone_motion: all tests passed\n");
    return 0;
}
