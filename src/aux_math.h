#pragma once

// Pure maths for the optional Qwiic sensors — no Arduino, no I2C, no state.
// Split out from aux_sensors.cpp so every formula here is exercised by
// tools/aux_test on the host: on the bike a wrong constant looks like "the
// altitude is a bit off", which is exactly the kind of wrong that ships.

#include <cmath>
#include <cstdint>

namespace aux_math {

// --- Barometric altitude ---------------------------------------------------

// ISA sea-level standard pressure, in pascals.
constexpr float SEA_LEVEL_PA = 101325.0f;

// Altitude (m) for a pressure, against a sea-level reference. The hypsometric
// form of the ISA troposphere model — the same one every altimeter uses:
//
//   h = (T0 / L) * (1 - (P/P0)^(R*L/g))     with T0=288.15 K, L=0.0065 K/m
//
// The exponent collapses to 0.190284 and the leading term to 44330.
//
// The REFERENCE is what makes this an altitude rather than a pressure reading.
// Against the standard 1013.25 hPa a real day is easily 100 m out, so callers
// are expected to set the reference from something that knows where it is (see
// seaLevelFor()). Weather still drifts it over hours, which is why the ride's
// ASCENT should come from this and its absolute height should be re-referenced
// whenever a better source is available.
inline float altitudeFromPressure(float pressurePa, float seaLevelPa) {
    if (!(pressurePa > 0.0f) || !(seaLevelPa > 0.0f)) return NAN;
    return 44330.0f * (1.0f - powf(pressurePa / seaLevelPa, 0.190284f));
}

// The inverse: the sea-level pressure implied by being at a KNOWN altitude
// while reading a given pressure. This is how the altimeter gets calibrated
// from the map's elevation grid (or a GPS altitude), instead of asking the
// rider for a QNH they do not have.
inline float seaLevelFor(float pressurePa, float knownAltM) {
    if (!(pressurePa > 0.0f)) return NAN;
    return pressurePa / powf(1.0f - knownAltM / 44330.0f, 5.255f);
}

// --- Magnetometer -----------------------------------------------------------

// Hard-iron offset, learned from the extremes seen while the device turns.
//
// A magnetometer on a bike reads the earth's field plus a fixed contribution
// from the steel and magnets around it (the bars, the mount, a dynamo). That
// offset is what makes an uncalibrated compass point confidently in the wrong
// direction, and no amount of maths recovers it from a single reading — it
// takes seeing the field from several directions, i.e. the rider turning.
//
// Min/max per axis is the cheap standard method: the offset is the centre of
// the box the readings sweep out. It needs no matrix work and self-corrects as
// the ride goes on. It does NOT correct soft-iron distortion (which scales and
// skews the sphere into an ellipsoid); that needs a fit this device has no
// reason to carry.
struct MagCal {
    float minX = 1e9f, minY = 1e9f, minZ = 1e9f;
    float maxX = -1e9f, maxY = -1e9f, maxZ = -1e9f;

    void observe(float x, float y, float z) {
        if (x < minX) minX = x;
        if (y < minY) minY = y;
        if (z < minZ) minZ = z;
        if (x > maxX) maxX = x;
        if (y > maxY) maxY = y;
        if (z > maxZ) maxZ = z;
    }

    float offsetX() const { return (maxX + minX) * 0.5f; }
    float offsetY() const { return (maxY + minY) * 0.5f; }
    float offsetZ() const { return (maxZ + minZ) * 0.5f; }

    // Enough of a spread on the two horizontal axes to trust a heading. Until
    // the rider has turned through a decent arc the centre estimate is just the
    // midpoint of whatever direction they happened to be pointing, and a
    // compass that is confidently wrong is worse than one that says nothing.
    // In microtesla; the earth's horizontal field is ~20-50 uT, so half of that
    // swept on both axes means a good fraction of a turn has been seen.
    bool ready(float minSpanUT = 20.0f) const {
        return (maxX - minX) >= minSpanUT && (maxY - minY) >= minSpanUT;
    }
};

// Tilt-compensated heading, in degrees clockwise from magnetic north.
//
// A bike computer sits at whatever angle the bars are at, and a compass that
// assumes it is flat swings tens of degrees as the device pitches. The gravity
// vector from the accelerometer says which way is down, so the field can be
// rotated back into the horizontal plane before the bearing is taken.
//
// Axes follow the LSM303AGR's own frame: x right, y forward, z up. Accelerometer
// values may be in any consistent unit (only their direction is used); magnetic
// values must already have the hard-iron offset removed.
inline float tiltCompensatedHeading(float ax, float ay, float az,
                                    float mx, float my, float mz) {
    const float norm = sqrtf(ax * ax + ay * ay + az * az);
    if (!(norm > 0.0f)) return NAN;
    ax /= norm; ay /= norm; az /= norm;

    // Into the frame the textbook formula below is written for — X forward,
    // Y right, Z DOWN — from the sensor's x right, y forward, z up. Doing this
    // mapping explicitly, once, is the difference between a compass that is
    // right and one that is a confident 90 degrees out with the sense reversed,
    // which is exactly what the first version of this did.
    const float Ax = ay, Ay = ax, Az = -az;
    const float Mx = my, My = mx, Mz = -mz;
    ax = Ax; ay = Ay; az = Az;
    mx = Mx; my = My; mz = Mz;

    // Pitch and roll from gravity. asinf's argument is clamped because a bump
    // can push a normalised component just past 1 and NAN the whole heading.
    float sinPitch = -ax;
    if (sinPitch > 1.0f) sinPitch = 1.0f;
    if (sinPitch < -1.0f) sinPitch = -1.0f;
    const float pitch = asinf(sinPitch);
    const float cosPitch = cosf(pitch);
    const float roll = (fabsf(cosPitch) > 1e-6f) ? asinf(ay / cosPitch) : 0.0f;

    const float sp = sinf(pitch), cp = cosf(pitch);
    const float sr = sinf(roll), cr = cosf(roll);

    const float xh = mx * cp + mz * sp;
    const float yh = mx * sr * sp + my * cr - mz * sr * cp;

    float deg = atan2f(-yh, xh) * 57.29577951f;
    if (deg < 0.0f) deg += 360.0f;
    return deg;
}

// Shortest signed difference between two bearings, in degrees (-180..180].
inline float headingDelta(float a, float b) {
    float d = fmodf(a - b + 540.0f, 360.0f) - 180.0f;
    return d;
}

// Learns the fixed yaw between what the magnetometer calls heading and the
// direction the bike is actually travelling.
//
// The compass reads in the SENSOR's frame, so it is only "forward" if the board
// happens to be mounted with its y axis down the top tube. Ours is on a Qwiic
// lead — it can be at any angle, on either side, upside down, and it will move
// the next time it is re-taped. Asking the rider to align it, or to enter an
// offset, is asking them to do arithmetic on a bike.
//
// GPS course-over-ground already knows the answer whenever the rider is moving:
// it IS the direction of travel. The difference between the two is the mounting
// yaw, and it is constant, so it can be averaged out of a noisy signal while
// riding and then applied when stopped — which is exactly when the compass is
// needed and course-over-ground is useless.
//
// It absorbs magnetic declination in the same number for free: GPS course is
// referenced to TRUE north and the magnetometer to magnetic, so whatever the
// local difference is, it lands in this offset without anyone shipping a
// declination table or knowing where in the world the bike is.
//
// Averaged as a unit vector rather than as an angle, because the mean of 359
// and 1 degrees is 0, not 180 — the bug every heading average has.
struct HeadingOffset {
    float sinAcc = 0.0f, cosAcc = 0.0f;
    int samples = 0;

    // `alpha` is the weight of each new sample: at 5 Hz, 0.05 settles in a few
    // seconds of riding and still ignores a single bad course.
    void observe(float compassDeg, float courseDeg, float alpha = 0.05f) {
        const float d = headingDelta(courseDeg, compassDeg) * 0.01745329f;
        sinAcc += alpha * (sinf(d) - sinAcc);
        cosAcc += alpha * (cosf(d) - cosAcc);
        if (samples < 1000000) samples++;
    }

    // Enough riding to trust it. Below this the offset is one noisy fix's
    // opinion, and applying that would make the compass worse than leaving it
    // in the sensor's own frame.
    bool ready(int minSamples = 50) const { return samples >= minSamples; }

    float offsetDeg() const {
        if (sinAcc == 0.0f && cosAcc == 0.0f) return 0.0f;
        float d = atan2f(sinAcc, cosAcc) * 57.29577951f;
        return d < 0.0f ? d + 360.0f : d;
    }

    // The corrected heading: what the rider is actually pointing at.
    float apply(float compassDeg) const {
        if (!ready()) return compassDeg;
        float h = fmodf(compassDeg + offsetDeg(), 360.0f);
        return h < 0.0f ? h + 360.0f : h;
    }

    // Seed from a value learned on a previous ride, so the compass is right
    // from the first second rather than after the rider has ridden far enough
    // to teach it again. Counted as just-ready: a real ride overrides it within
    // seconds if the device has been re-mounted since.
    void seed(float offDeg, int asSamples = 50) {
        const float r = offDeg * 0.01745329f;
        sinAcc = sinf(r);
        cosAcc = cosf(r);
        samples = asSamples;
    }
};

// --- Movement ---------------------------------------------------------------

// Is the device moving, from the spread of recent acceleration magnitudes?
//
// Gravity dominates any single sample, so its ABSOLUTE value says nothing about
// motion — a device lying still reads 1 g just as one held steady in a moving
// car does. What separates them is the wobble: road buzz, pedalling, the bars
// moving. So this watches the peak-to-peak spread over a short window and calls
// it movement when that exceeds a threshold.
//
// Deliberately not a tilt/orientation test: a bike computer that is bumped or
// re-angled has not started riding, and one held perfectly steady on a smooth
// road has.
struct MovementDetector {
    static constexpr int N = 16;          // ~3 s at 5 Hz
    float mag[N] = {0};
    int count = 0, head = 0;
    bool moving = false;

    // thresholdG: peak-to-peak, in g. Road buzz on tarmac is a few hundredths;
    // a device sitting on a desk is under 0.01 g of noise.
    void observe(float ax, float ay, float az, float thresholdG = 0.05f) {
        mag[head] = sqrtf(ax * ax + ay * ay + az * az);
        head = (head + 1) % N;
        if (count < N) count++;
        if (count < N) return;            // no verdict until the window is full

        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < N; ++i) {
            if (mag[i] < lo) lo = mag[i];
            if (mag[i] > hi) hi = mag[i];
        }
        const float span = hi - lo;
        // Hysteresis: starting takes the full threshold, stopping takes falling
        // to half of it, so a rider soft-pedalling on smooth tarmac does not
        // flicker between states.
        moving = span >= (moving ? thresholdG * 0.5f : thresholdG);
    }

    void reset() { count = 0; head = 0; moving = false; }
};

}  // namespace aux_math
