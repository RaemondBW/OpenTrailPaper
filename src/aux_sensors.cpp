#include "aux_sensors.h"

#include <Arduino.h>
#include <Wire.h>
#include <cstring>

#include "aux_math.h"
#include "config.h"
#include "diag.h"
#include "i2c_bus.h"
#include "ride_state.h"
#include "settings.h"

// Hand-rolled drivers rather than pulling in two more libraries: both chips are
// a handful of registers, the vendored SensorLib covers neither, and every
// access on this bus has to go through i2cLock() — which a stock library will
// not do (see the panel driver, whose unguarded traffic on this same bus is
// what corrupts other people's reads).

namespace {

constexpr uint8_t BME280_ADDR_A = 0x76;
constexpr uint8_t BME280_ADDR_B = 0x77;
constexpr uint8_t BME280_REG_ID = 0xD0;
constexpr uint8_t BME280_CHIP_ID = 0x60;      // 0x58 is a BMP280: no humidity,
                                              // but pressure works identically
constexpr uint8_t BMP280_CHIP_ID = 0x58;
constexpr uint8_t BME280_REG_RESET = 0xE0;
constexpr uint8_t BME280_REG_CTRL_MEAS = 0xF4;
constexpr uint8_t BME280_REG_CONFIG = 0xF5;
constexpr uint8_t BME280_REG_PRESS = 0xF7;
constexpr uint8_t BME280_REG_CALIB = 0x88;

constexpr uint8_t LSM_ACC_ADDR = 0x19;
constexpr uint8_t LSM_MAG_ADDR = 0x1E;
constexpr uint8_t LSM_WHO_AM_I_A = 0x0F;      // reads 0x33
constexpr uint8_t LSM_WHO_AM_I_M = 0x4F;      // reads 0x40
constexpr uint8_t LSM_CTRL_REG1_A = 0x20;
constexpr uint8_t LSM_CTRL_REG4_A = 0x23;
constexpr uint8_t LSM_OUT_X_L_A = 0x28;
constexpr uint8_t LSM_CFG_REG_A_M = 0x60;
constexpr uint8_t LSM_OUTX_L_REG_M = 0x68;

bool baroOk = false, accOk = false, magOk = false;
uint8_t baroAddr = 0;

// BME280 factory calibration, read once at begin().
struct {
    uint16_t T1; int16_t T2, T3;
    uint16_t P1; int16_t P2, P3, P4, P5, P6, P7, P8, P9;
} cal;
int32_t tFine = 0;

aux_math::MagCal magCal;
aux_math::MovementDetector movement;
aux_math::HeadingOffset mountOffset;
float savedOffset = NAN;

// The altimeter's reference. Starts at the ISA standard — good enough to be a
// plausible number — and is re-solved from the map's elevation grid the first
// time the rider is somewhere the DEM knows about, and then only slowly.
float seaLevel = aux_math::SEA_LEVEL_PA;
bool seaLevelFromDem = false;
uint32_t lastRefMs = 0;

// --- I2C helpers. Every one takes the bus lock; none of them retries, because
// a dropped sample here costs nothing — the next one is 200 ms away.

bool readRegs(uint8_t addr, uint8_t reg, uint8_t* buf, size_t n) {
    i2cLock();
    Wire.beginTransmission(addr);
    Wire.write(reg);
    bool ok = Wire.endTransmission(false) == 0 &&
              Wire.requestFrom((int)addr, (int)n) == (int)n;
    if (ok) for (size_t i = 0; i < n; ++i) buf[i] = Wire.read();
    i2cUnlock();
    return ok;
}

bool writeReg(uint8_t addr, uint8_t reg, uint8_t val) {
    i2cLock();
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(val);
    bool ok = Wire.endTransmission() == 0;
    i2cUnlock();
    return ok;
}

// Three goes before calling a chip absent.
//
// This bus is shared with a panel driver that does its own I2C on a private
// mutex, outside i2cLock() (see board_side_button_pressed for the same fault
// biting a button read). A collision during a single probe would brand a
// perfectly good sensor "not found" for the whole session, and boot is exactly
// when the panel is busiest. A retry costs a few hundred microseconds.
bool probe(uint8_t addr, uint8_t whoReg, uint8_t expect) {
    for (int i = 0; i < 3; ++i) {
        uint8_t v = 0;
        if (readRegs(addr, whoReg, &v, 1) && v == expect) return true;
        delay(2);
    }
    return false;
}

// --- BME280 ---------------------------------------------------------------

bool baroBegin(uint8_t addr) {
    // Same retry as probe(): one unlucky read must not cost the barometer.
    uint8_t id = 0;
    bool got = false;
    for (int i = 0; i < 3 && !got; ++i) {
        if (readRegs(addr, BME280_REG_ID, &id, 1) &&
            (id == BME280_CHIP_ID || id == BMP280_CHIP_ID)) {
            got = true;
        } else {
            delay(2);
        }
    }
    if (!got) return false;

    uint8_t c[24];
    if (!readRegs(addr, BME280_REG_CALIB, c, sizeof(c))) return false;
    cal.T1 = (uint16_t)(c[0] | (c[1] << 8));
    cal.T2 = (int16_t)(c[2] | (c[3] << 8));
    cal.T3 = (int16_t)(c[4] | (c[5] << 8));
    cal.P1 = (uint16_t)(c[6] | (c[7] << 8));
    cal.P2 = (int16_t)(c[8] | (c[9] << 8));
    cal.P3 = (int16_t)(c[10] | (c[11] << 8));
    cal.P4 = (int16_t)(c[12] | (c[13] << 8));
    cal.P5 = (int16_t)(c[14] | (c[15] << 8));
    cal.P6 = (int16_t)(c[16] | (c[17] << 8));
    cal.P7 = (int16_t)(c[18] | (c[19] << 8));
    cal.P8 = (int16_t)(c[20] | (c[21] << 8));
    cal.P9 = (int16_t)(c[22] | (c[23] << 8));

    // IIR filter x16 and 500 ms standby: this is an altimeter, not a barometer
    // logger, and the filter is what stops a gust or a slammed door showing up
    // as a metre of climb. Then normal mode, pressure x16, temperature x2 —
    // temperature only feeds the pressure compensation here.
    if (!writeReg(addr, BME280_REG_CONFIG, (0x04 << 5) | (0x04 << 2))) return false;
    if (!writeReg(addr, BME280_REG_CTRL_MEAS, (0x02 << 5) | (0x05 << 2) | 0x03))
        return false;
    baroAddr = addr;
    return true;
}

// Bosch's fixed-point compensation, transcribed from the datasheet (sections
// 4.2.3 / 8.1). Kept in integer form on purpose — the float version of the
// pressure formula loses enough precision to wobble the altitude by a metre,
// which on a barometric altimeter is the entire point of having one.
int32_t compensateT(int32_t adcT) {
    int32_t v1 = ((((adcT >> 3) - ((int32_t)cal.T1 << 1))) * ((int32_t)cal.T2)) >> 11;
    int32_t v2 = (((((adcT >> 4) - ((int32_t)cal.T1)) *
                    ((adcT >> 4) - ((int32_t)cal.T1))) >> 12) *
                  ((int32_t)cal.T3)) >> 14;
    tFine = v1 + v2;
    return (tFine * 5 + 128) >> 8;             // centi-degrees C
}

uint32_t compensateP(int32_t adcP) {
    int64_t v1 = ((int64_t)tFine) - 128000;
    int64_t v2 = v1 * v1 * (int64_t)cal.P6;
    v2 = v2 + ((v1 * (int64_t)cal.P5) << 17);
    v2 = v2 + (((int64_t)cal.P4) << 35);
    v1 = ((v1 * v1 * (int64_t)cal.P3) >> 8) + ((v1 * (int64_t)cal.P2) << 12);
    v1 = (((((int64_t)1) << 47) + v1)) * ((int64_t)cal.P1) >> 33;
    if (v1 == 0) return 0;                     // datasheet: avoid /0
    int64_t p = 1048576 - adcP;
    p = (((p << 31) - v2) * 3125) / v1;
    v1 = (((int64_t)cal.P9) * (p >> 13) * (p >> 13)) >> 25;
    v2 = (((int64_t)cal.P8) * p) >> 19;
    p = ((p + v1 + v2) >> 8) + (((int64_t)cal.P7) << 4);
    return (uint32_t)p;                        // Q24.8 pascals
}

bool baroRead(float& pressurePa, float& tempC) {
    uint8_t d[6];
    if (!readRegs(baroAddr, BME280_REG_PRESS, d, sizeof(d))) return false;
    int32_t adcP = ((int32_t)d[0] << 12) | ((int32_t)d[1] << 4) | (d[2] >> 4);
    int32_t adcT = ((int32_t)d[3] << 12) | ((int32_t)d[4] << 4) | (d[5] >> 4);
    if (adcP == 0x80000 || adcT == 0x80000) return false;   // sensor not ready
    tempC = compensateT(adcT) / 100.0f;
    uint32_t p = compensateP(adcP);
    if (p == 0) return false;
    pressurePa = p / 256.0f;
    return true;
}

// --- LSM303AGR ------------------------------------------------------------

bool accBegin() {
    if (!probe(LSM_ACC_ADDR, LSM_WHO_AM_I_A, 0x33)) return false;
    // 50 Hz, all axes, normal power mode. We only sample at 5 Hz, but a faster
    // ODR means each sample is fresh rather than up to a period stale, and the
    // chip's own filtering behaves.
    if (!writeReg(LSM_ACC_ADDR, LSM_CTRL_REG1_A, 0x47)) return false;
    // +/-2 g, high resolution (12-bit).
    return writeReg(LSM_ACC_ADDR, LSM_CTRL_REG4_A, 0x08);
}

bool magBegin() {
    if (!probe(LSM_MAG_ADDR, LSM_WHO_AM_I_M, 0x40)) return false;
    // Continuous mode, 20 Hz, temperature compensation on (bit 7) — the mag's
    // sensitivity drifts with temperature and the chip corrects it for free.
    return writeReg(LSM_MAG_ADDR, LSM_CFG_REG_A_M, 0x8C);
}

// Raw accelerometer in g. The AGR reports left-justified 12-bit at +/-2 g, so
// the useful value is the top 12 bits and one count is 0.98 mg.
bool accRead(float& x, float& y, float& z) {
    uint8_t d[6];
    // Bit 7 of the sub-address is the auto-increment flag on this part; without
    // it every byte of the burst comes back from the same register.
    if (!readRegs(LSM_ACC_ADDR, LSM_OUT_X_L_A | 0x80, d, sizeof(d))) return false;
    int16_t rx = (int16_t)(d[0] | (d[1] << 8));
    int16_t ry = (int16_t)(d[2] | (d[3] << 8));
    int16_t rz = (int16_t)(d[4] | (d[5] << 8));
    x = (rx >> 4) * 0.000976f;
    y = (ry >> 4) * 0.000976f;
    z = (rz >> 4) * 0.000976f;
    return true;
}

// Raw magnetometer in microtesla (1 LSB = 1.5 mgauss = 0.15 uT).
bool magRead(float& x, float& y, float& z) {
    uint8_t d[6];
    if (!readRegs(LSM_MAG_ADDR, LSM_OUTX_L_REG_M, d, sizeof(d))) return false;
    x = (int16_t)(d[0] | (d[1] << 8)) * 0.15f;
    y = (int16_t)(d[2] | (d[3] << 8)) * 0.15f;
    z = (int16_t)(d[4] | (d[5] << 8)) * 0.15f;
    return true;
}

}  // namespace

namespace aux_sensors {

bool begin() {
    baroOk = baroBegin(BME280_ADDR_A) || baroBegin(BME280_ADDR_B);
    accOk = accBegin();
    magOk = accOk && magBegin();   // no compass without gravity to level it

    // Last ride's mounting yaw. The board has not moved since, so the compass
    // is correct from the first second instead of after enough riding to learn
    // it again — and if it HAS been re-mounted, a minute of riding overrides it.
    savedOffset = settings::compassOffsetDeg();
    if (magOk && !isnan(savedOffset)) mountOffset.seed(savedOffset);

    if (!baroOk && !accOk) {
        diag::log("aux: no Qwiic sensors found (BME280 0x76/0x77, LSM303AGR 0x19/0x1E)");
        return false;
    }
    diag::log("aux: baro %s%s, accel %s, mag %s", baroOk ? "OK @0x" : "absent",
              baroOk ? (baroAddr == BME280_ADDR_A ? "76" : "77") : "",
              accOk ? "OK" : "absent", magOk ? "OK" : "absent");
    return true;
}

bool haveBaro() { return baroOk; }
bool haveCompass() { return magOk; }
bool haveMotion() { return accOk; }
uint8_t baroAddress() { return baroOk ? baroAddr : 0; }
float seaLevelPa() { return seaLevel; }
bool seaLevelCalibrated() { return seaLevelFromDem; }

void task(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));        // 5 Hz
        RideState s = g_state.snapshot();

        if (baroOk) {
            float pa = 0, tc = 0;
            if (baroRead(pa, tc)) {
                // Re-reference against the map's elevation grid. The DEM knows
                // the ground height where the rider is; the barometer knows how
                // the pressure has changed since. Solving for sea level ties the
                // two together, and re-solving occasionally follows the weather
                // — every 5 minutes, because doing it continuously would let the
                // DEM's own steps become the altitude and throw away the very
                // resolution the barometer was added for.
                const uint32_t now = millis();
                if (s.mapElevationValid &&
                    (!seaLevelFromDem || now - lastRefMs > 300000)) {
                    float ref = aux_math::seaLevelFor(pa, s.mapElevationM);
                    if (ref > 87000.0f && ref < 108500.0f) {   // sane QNH range
                        seaLevel = ref;
                        seaLevelFromDem = true;
                        lastRefMs = now;
                    }
                }
                const float alt = aux_math::altitudeFromPressure(pa, seaLevel);
                g_state.with([&](RideState& st) {
                    st.pressurePa = pa;
                    st.baroTempC = tc;
                    st.baroAltM = alt;
                    st.baroValid = !isnan(alt);
                });
            }
        }

        float ax = 0, ay = 0, az = 0;
        bool haveAcc = accOk && accRead(ax, ay, az);
        if (haveAcc) {
            movement.observe(ax, ay, az);
            g_state.with([&](RideState& st) { st.deviceMoving = movement.moving; });
        }

        if (magOk && haveAcc) {
            float mx = 0, my = 0, mz = 0;
            if (magRead(mx, my, mz)) {
                magCal.observe(mx, my, mz);
                if (magCal.ready()) {
                    float raw = aux_math::tiltCompensatedHeading(
                        ax, ay, az, mx - magCal.offsetX(), my - magCal.offsetY(),
                        mz - magCal.offsetZ());
                    if (!isnan(raw)) {
                        // Riding is what teaches the mounting yaw: above 8 km/h
                        // with a fix, course-over-ground IS the way the bike
                        // points, so the gap between the two is how the board
                        // sits — plus the local magnetic declination, which
                        // lands in the same number for free.
                        if (s.gpsFix && s.speedKmh > 8.0f)
                            mountOffset.observe(raw, s.courseDeg);

                        const float h = mountOffset.apply(raw);
                        g_state.with([&](RideState& st) {
                            st.compassDeg = h;
                            st.compassValid = true;
                        });

                        // Persist once it has drifted enough to be worth a write
                        // — this is a slowly-learned constant and NVS has a
                        // finite erase budget.
                        if (mountOffset.ready()) {
                            const float now = mountOffset.offsetDeg();
                            if (isnan(savedOffset) ||
                                fabsf(aux_math::headingDelta(now, savedOffset)) > 5.0f) {
                                savedOffset = now;
                                settings::setCompassOffsetDeg(now);
                            }
                        }
                    }
                }
            }
        }
    }
}

void report(char* out, size_t n) {
    RideState s = g_state.snapshot();
    snprintf(out, n,
             "baro %s %.1fhPa %.1fm %.1fC (QNH %.1f%s) · acc %s%s · mag %s",
             baroOk ? "on" : "off", s.pressurePa / 100.0f, s.baroAltM, s.baroTempC,
             seaLevel / 100.0f, seaLevelFromDem ? " from DEM" : " default",
             accOk ? "on" : "off", s.deviceMoving ? " MOVING" : "",
             !magOk ? "off" : (s.compassValid ? "ready" : "calibrating"));
    if (magOk) {
        const size_t used = strlen(out);
        snprintf(out + used, n > used ? n - used : 0, " (mount %+.0f deg, %s)",
                 mountOffset.offsetDeg(),
                 mountOffset.ready() ? "learned" : "learning — ride to set it");
    }
}

}  // namespace aux_sensors
