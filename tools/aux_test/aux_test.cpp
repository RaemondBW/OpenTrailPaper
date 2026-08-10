// Host tests for src/aux_math.h — the maths behind the optional Qwiic sensors.
//
// These formulas are the kind that are wrong quietly: a bad exponent still
// produces an altitude that rises as you climb, just one that is 15% off, and
// nobody spots that on a bike. So they are checked against values that can be
// looked up independently (the ISA standard atmosphere) rather than against
// whatever this implementation happens to produce.

#include <cstdio>
#include <cmath>
#include <string>

#include "aux_math.h"

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

// --- Barometric altitude ---------------------------------------------------

// The ISA standard atmosphere, from the reference tables. If the exponent or
// the scale height is wrong these are the numbers that catch it.
void testStandardAtmosphere() {
    begin("ISA table: pressure -> altitude");
    using aux_math::altitudeFromPressure;
    const float P0 = aux_math::SEA_LEVEL_PA;

    checkNear(altitudeFromPressure(101325.0f, P0), 0.0f, 0.5f, "1013.25 hPa is sea level");
    checkNear(altitudeFromPressure(100000.0f, P0), 110.9f, 2.0f, "1000 hPa is ~111 m");
    checkNear(altitudeFromPressure(95461.0f, P0), 500.0f, 5.0f, "954.61 hPa is 500 m");
    checkNear(altitudeFromPressure(89876.0f, P0), 1000.0f, 8.0f, "898.76 hPa is 1000 m");
    checkNear(altitudeFromPressure(79501.0f, P0), 2000.0f, 15.0f, "795.01 hPa is 2000 m");
}

// One hPa is very close to 8.4 m near sea level — the rule of thumb every
// altimeter obeys, and a sanity check on the derivative rather than the value.
void testMetresPerHectopascal() {
    begin("resolution: ~8.4 m per hPa near sea level");
    using aux_math::altitudeFromPressure;
    const float P0 = aux_math::SEA_LEVEL_PA;
    float a = altitudeFromPressure(101325.0f, P0);
    float b = altitudeFromPressure(101225.0f, P0);   // 1 hPa lower
    checkNear(b - a, 8.4f, 0.5f, "one hectopascal of climb");
}

// The calibration path: at a known elevation, solving for sea level and then
// converting back has to return the elevation you started from.
void testSeaLevelRoundTrip() {
    begin("calibration: solve for sea level, get the altitude back");
    for (float knownAlt : {0.0f, 50.0f, 250.0f, 1200.0f, 2500.0f}) {
        // Whatever the barometer would read at that height on a standard day.
        float pressure = aux_math::SEA_LEVEL_PA *
                         powf(1.0f - knownAlt / 44330.0f, 5.255f);
        float ref = aux_math::seaLevelFor(pressure, knownAlt);
        float back = aux_math::altitudeFromPressure(pressure, ref);
        char what[80];
        snprintf(what, sizeof(what), "round trip at %.0f m", knownAlt);
        checkNear(back, knownAlt, 1.0f, what);
    }
}

// A low-pressure day must not read as a mountain. This is the whole reason the
// reference is calibrated from the elevation grid rather than assumed.
void testWeatherOffsetIsCorrected() {
    begin("weather: an uncalibrated reference is wrong by tens of metres");
    const float trueAlt = 100.0f;
    const float qnh = 99000.0f;                  // a deep low, ~990 hPa
    float pressure = qnh * powf(1.0f - trueAlt / 44330.0f, 5.255f);

    float uncalibrated = aux_math::altitudeFromPressure(pressure, aux_math::SEA_LEVEL_PA);
    check(uncalibrated > trueAlt + 150.0f,
          "uncalibrated reads high on a low-pressure day (that is the bug)");
    float calibrated = aux_math::altitudeFromPressure(
        pressure, aux_math::seaLevelFor(pressure, trueAlt));
    checkNear(calibrated, trueAlt, 1.0f, "calibrated against a known elevation");
}

// --- Compass ---------------------------------------------------------------

// Flat and level, the tilt compensation must not change anything: the heading
// is just the bearing of the horizontal field.
void testHeadingFlat() {
    begin("compass: level device reads the field bearing");
    using aux_math::tiltCompensatedHeading;
    // Gravity is -1 g on z (z up). Field pointing along +y = north = 0 deg.
    checkNear(tiltCompensatedHeading(0, 0, -1, 0, 20, -40), 0.0f, 1.0f, "north");
    checkNear(tiltCompensatedHeading(0, 0, -1, -20, 0, -40), 90.0f, 1.0f, "east");
    checkNear(tiltCompensatedHeading(0, 0, -1, 0, -20, -40), 180.0f, 1.0f, "south");
    checkNear(tiltCompensatedHeading(0, 0, -1, 20, 0, -40), 270.0f, 1.0f, "west");
}

// THE reason tilt compensation exists: a device pitched on the bars must report
// the same heading as one lying flat. Without the correction this swings tens
// of degrees, which is what makes an uncompensated bike compass useless.
void testHeadingSurvivesPitch() {
    begin("compass: heading holds while the device is pitched");
    using aux_math::tiltCompensatedHeading;
    // Northern-hemisphere field: 20 uT north, and a vertical part pointing DOWN
    // (negative z in this frame) at 60 degrees of inclination.
    const float incl = 60.0f * (float)M_PI / 180.0f;
    const float Bh = 20.0f, Bv = -Bh * tanf(incl);

    float flat = tiltCompensatedHeading(0, 0, -1, 0, Bh, Bv);
    checkNear(flat, 0.0f, 1.0f, "level, facing north");

    // Pitch the device nose-up about its x axis. BOTH gravity and the field
    // have to be rotated into the tilted frame, or the test is measuring
    // nothing: v_dev = Rx(-p) * v_level.
    for (float pitchDeg : {10.0f, 25.0f, 40.0f}) {
        const float p = pitchDeg * (float)M_PI / 180.0f;
        const float ay = -sinf(p), az = -cosf(p);          // gravity (0,0,-1)
        const float my = Bh * cosf(p) + Bv * sinf(p);
        const float mz = -Bh * sinf(p) + Bv * cosf(p);
        float tilted = tiltCompensatedHeading(0, ay, az, 0, my, mz);
        char what[80];
        snprintf(what, sizeof(what), "pitched %.0f deg still reads north",
                 pitchDeg);
        check(fabsf(aux_math::headingDelta(tilted, flat)) < 5.0f, what);
    }
}

void testHeadingDelta() {
    begin("compass: shortest way round");
    checkNear(aux_math::headingDelta(10, 350), 20.0f, 0.01f, "10 vs 350 is +20");
    checkNear(aux_math::headingDelta(350, 10), -20.0f, 0.01f, "350 vs 10 is -20");
    checkNear(aux_math::headingDelta(90, 90), 0.0f, 0.01f, "same bearing");
}

// --- Mounting-yaw offset ----------------------------------------------------

// The whole point: a board taped on at some arbitrary angle must still give a
// heading that means "the way the bike is pointing".
void testHeadingOffsetLearnsMounting() {
    begin("mounting yaw: learned from GPS course while riding");
    aux_math::HeadingOffset off;
    check(!off.ready(), "not ready before it has seen any riding");
    // The board is rotated 37 deg from forward, so it reads 37 low.
    const float mounting = 37.0f;
    for (int i = 0; i < 200; ++i) {
        float trueCourse = fmodf(20.0f + i * 1.3f, 360.0f);   // riding a curve
        float compass = fmodf(trueCourse - mounting + 360.0f, 360.0f);
        off.observe(compass, trueCourse);
    }
    check(off.ready(), "ready after riding");
    checkNear(off.offsetDeg(), mounting, 3.0f, "offset recovered");
    // And applying it turns a sensor-frame reading into a real bearing.
    checkNear(off.apply(fmodf(90.0f - mounting + 360.0f, 360.0f)), 90.0f, 3.0f,
              "corrected heading points where the bike does");
}

// A mounting angle that straddles the 0/360 wrap is where an angle-averaging
// version falls over: the mean of 359 and 1 is 0, not 180.
void testHeadingOffsetAcrossWrap() {
    begin("mounting yaw: survives the 0/360 wrap");
    aux_math::HeadingOffset off;
    const float mounting = 358.0f;
    for (int i = 0; i < 200; ++i) {
        float trueCourse = fmodf(i * 2.7f, 360.0f);
        float compass = fmodf(trueCourse - mounting + 720.0f, 360.0f);
        off.observe(compass, trueCourse);
    }
    check(fabsf(aux_math::headingDelta(off.offsetDeg(), mounting)) < 3.0f,
          "offset near 360 recovered without wrapping to the middle");
}

// Until it has learned anything, it must pass the reading straight through
// rather than applying half an opinion.
void testHeadingOffsetPassesThroughUntilReady() {
    begin("mounting yaw: no correction before it is trusted");
    aux_math::HeadingOffset off;
    for (int i = 0; i < 5; ++i) off.observe(100.0f, 140.0f);
    check(!off.ready(), "five samples is not a calibration");
    checkNear(off.apply(123.0f), 123.0f, 0.01f, "reading passes through");
}

// A stored offset from a previous ride applies immediately.
void testHeadingOffsetSeed() {
    begin("mounting yaw: a stored offset applies from the first second");
    aux_math::HeadingOffset off;
    off.seed(90.0f);
    check(off.ready(), "seeded is ready");
    checkNear(off.apply(0.0f), 90.0f, 0.01f, "seeded offset applied");
}

// --- Hard-iron calibration --------------------------------------------------

void testMagCal() {
    begin("hard iron: the offset is the centre of what was seen");
    aux_math::MagCal c;
    check(!c.ready(), "not ready before it has seen anything");
    // A field of 30 uT swept in a full circle, offset by (12, -7).
    for (int i = 0; i < 360; i += 10) {
        float a = i * (float)M_PI / 180.0f;
        c.observe(12.0f + 30.0f * cosf(a), -7.0f + 30.0f * sinf(a), 5.0f);
    }
    check(c.ready(), "ready after a full turn");
    checkNear(c.offsetX(), 12.0f, 0.5f, "x offset recovered");
    checkNear(c.offsetY(), -7.0f, 0.5f, "y offset recovered");

    // A device that only ever pointed one way must NOT claim to be calibrated.
    aux_math::MagCal straight;
    for (int i = 0; i < 50; ++i) straight.observe(12.0f + 0.1f * i, -7.0f, 5.0f);
    check(!straight.ready(), "pointing one way is not a calibration");
}

// --- Movement ---------------------------------------------------------------

void testMovement() {
    begin("movement: still is still, buzz is moving");
    aux_math::MovementDetector d;
    // A device on a desk: 1 g and sensor noise.
    for (int i = 0; i < 40; ++i) d.observe(0.001f * (i % 3), 0.0f, 1.0f + 0.002f * (i % 2));
    check(!d.moving, "sitting on a desk is not movement");

    // Riding: gravity plus road buzz.
    for (int i = 0; i < 40; ++i) {
        float buzz = ((i % 4) - 1.5f) * 0.08f;
        d.observe(buzz, buzz * 0.5f, 1.0f + buzz);
    }
    check(d.moving, "road buzz is movement");

    // And it lets go again once the bike is parked.
    for (int i = 0; i < 40; ++i) d.observe(0.0f, 0.0f, 1.0f);
    check(!d.moving, "stops reporting movement once parked");
}

}  // namespace

int main() {
    printf("aux sensor maths tests\n");
    testStandardAtmosphere();
    testMetresPerHectopascal();
    testSeaLevelRoundTrip();
    testWeatherOffsetIsCorrected();
    testHeadingFlat();
    testHeadingSurvivesPitch();
    testHeadingDelta();
    testHeadingOffsetLearnsMounting();
    testHeadingOffsetAcrossWrap();
    testHeadingOffsetPassesThroughUntilReady();
    testHeadingOffsetSeed();
    testMagCal();
    testMovement();

    if (g_fail == 0) { printf("all aux tests passed\n"); return 0; }
    printf("%d aux assertion(s) FAILED\n", g_fail);
    return 1;
}
