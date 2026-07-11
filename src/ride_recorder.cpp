#include "ride_recorder.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include "config.h"
#include "fit_writer.h"

namespace {

FitWriter fit;
bool sdOk = false;
char ridePath[48];

double lastLat = 0, lastLon = 0;
bool havePrevFix = false;
double distanceM = 0;
uint32_t timerS = 0;
uint32_t lastFlushS = 0;

// Stats for the summary screen
uint32_t movingS = 0;
time_t startUtc = 0, endUtc = 0;
uint64_t powerSum = 0;
uint32_t powerCount = 0;
uint64_t hrSum = 0;
uint32_t hrCount = 0;
double climbedM = 0;
float climbBaseAlt = 0;
bool climbBaseValid = false;

// Normalized power: 30 s rolling average, 4th-power mean
uint16_t npRing[30];
int npRingCount = 0, npRingHead = 0;
double npSum4 = 0;
uint32_t npCount = 0;

// Grade: altitude change over the last ~30 m of travel
double gradeMarkDist = 0;
float gradeMarkAlt = 0;
bool gradeMarkValid = false;

double haversineM(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371000.0;
    double dLat = radians(lat2 - lat1);
    double dLon = radians(lon2 - lon1);
    double a = sin(dLat / 2) * sin(dLat / 2) +
               cos(radians(lat1)) * cos(radians(lat2)) *
               sin(dLon / 2) * sin(dLon / 2);
    return R * 2 * atan2(sqrt(a), sqrt(1 - a));
}

void makeRidePath(char* out, size_t len, time_t utc) {
    struct tm tmv;
    gmtime_r(&utc, &tmv);
    snprintf(out, len, RIDE_DIR "/%04d%02d%02d-%02d%02d%02d.fit",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

void resetStats() {
    distanceM = 0;
    timerS = 0;
    movingS = 0;
    lastFlushS = 0;
    havePrevFix = false;
    powerSum = powerCount = 0;
    hrSum = hrCount = 0;
    climbedM = 0;
    climbBaseValid = false;
    npRingCount = npRingHead = 0;
    npSum4 = 0;
    npCount = 0;
    gradeMarkValid = false;
    gradeMarkDist = 0;
}

void accumulateStats(const RideState& s) {
    if (s.speedKmh > 3.0f) movingS++;

    if (s.powerW != 0xFFFF) {
        powerSum += s.powerW;
        powerCount++;
        // Normalized power: mean of (30 s rolling avg)^4, 4th root
        npRing[npRingHead] = s.powerW;
        npRingHead = (npRingHead + 1) % 30;
        if (npRingCount < 30) npRingCount++;
        if (npRingCount == 30) {
            uint32_t sum = 0;
            for (int i = 0; i < 30; ++i) sum += npRing[i];
            double avg = sum / 30.0;
            npSum4 += avg * avg * avg * avg;
            npCount++;
        }
    }
    if (s.heartRateBpm != 0xFF) {
        hrSum += s.heartRateBpm;
        hrCount++;
    }

    // Climb: accumulate ascent with 3 m hysteresis against GPS noise
    if (s.gpsFix) {
        if (!climbBaseValid) {
            climbBaseAlt = s.altitudeM;
            climbBaseValid = true;
        } else if (s.altitudeM > climbBaseAlt + 3.0f) {
            climbedM += s.altitudeM - climbBaseAlt;
            climbBaseAlt = s.altitudeM;
        } else if (s.altitudeM < climbBaseAlt - 3.0f) {
            climbBaseAlt = s.altitudeM;
        }
    }
}

// GPS altitude is very noisy (±10-30 m with no barometer), so a short
// baseline makes grade jump around randomly. We heavily smooth the
// altitude and measure the rise over a long (100 m) baseline, then clamp
// to a sane range and slew-limit the output. Result is a stable, if
// coarse, grade instead of noise.
float gradeSmoothAlt = 0;
bool gradeAltPrimed = false;
float gradeOut = 0;

float updateGrade(float altitudeM) {
    // EMA on altitude first — kills per-fix jitter.
    if (!gradeAltPrimed) {
        gradeSmoothAlt = altitudeM;
        gradeAltPrimed = true;
    } else {
        gradeSmoothAlt += 0.15f * (altitudeM - gradeSmoothAlt);
    }

    if (!gradeMarkValid) {
        gradeMarkDist = distanceM;
        gradeMarkAlt = gradeSmoothAlt;
        gradeMarkValid = true;
        return NAN;
    }

    double dDist = distanceM - gradeMarkDist;
    if (dDist < 100.0) return NAN;  // need a long baseline; keep last value

    float raw = (gradeSmoothAlt - gradeMarkAlt) / (float)dDist * 100.0f;
    if (raw > 25.0f) raw = 25.0f;
    if (raw < -25.0f) raw = -25.0f;
    // Slew-limit so the shown number eases toward the new estimate.
    gradeOut += 0.5f * (raw - gradeOut);

    gradeMarkDist = distanceM;
    gradeMarkAlt = gradeSmoothAlt;
    return gradeOut;
}

}  // namespace

namespace ride_recorder {

bool begin() {
    // SD and LoRa share the SPI bus; a floating LoRa CS corrupts SD traffic.
    pinMode(BOARD_LORA_CS, OUTPUT);
    digitalWrite(BOARD_LORA_CS, HIGH);
    pinMode(BOARD_SD_CS, OUTPUT);
    digitalWrite(BOARD_SD_CS, HIGH);

    SPI.begin(BOARD_SPI_SCLK, BOARD_SPI_MISO, BOARD_SPI_MOSI);
    sdOk = SD.begin(BOARD_SD_CS);
    if (!sdOk) {
        Serial.println("[rec] SD mount failed — recording disabled");
        return false;
    }
    if (!SD.exists(RIDE_DIR)) SD.mkdir(RIDE_DIR);
    Serial.printf("[rec] SD ready, %llu MB free\n",
                  (SD.totalBytes() - SD.usedBytes()) / (1024ULL * 1024ULL));
    return true;
}

void startRide() {
    if (!sdOk || fit.isOpen()) return;

    RideState s = g_state.snapshot();
    if (!s.timeValid) {
        Serial.println("[rec] no GPS time yet — can't start ride");
        return;
    }

    makeRidePath(ridePath, sizeof(ridePath), s.utc);
    if (!fit.begin(SD, ridePath, s.utc)) {
        Serial.printf("[rec] failed to open %s\n", ridePath);
        return;
    }

    startUtc = s.utc;
    resetStats();
    g_state.with([](RideState& st) {
        st.recording = true;
        st.distanceM = 0;
        st.elapsedS = 0;
        st.movingS = 0;
        st.climbedM = 0;
        st.gradeValid = false;
    });
    Serial.printf("[rec] recording to %s\n", ridePath);
}

void stopRide(bool save) {
    if (!fit.isOpen()) return;
    RideState s = g_state.snapshot();
    endUtc = s.timeValid ? s.utc : startUtc + timerS;
    fit.finish(endUtc, distanceM, timerS);
    if (!save) {
        SD.remove(ridePath);
        Serial.println("[rec] ride discarded");
    } else {
        Serial.printf("[rec] ride saved: %.1f km, %lu s\n", distanceM / 1000.0,
                      (unsigned long)timerS);
    }
    g_state.with([](RideState& st) { st.recording = false; });
}

bool isRecording() { return fit.isOpen(); }

RideSummary summary() {
    RideSummary r;
    r.distanceM = distanceM;
    r.movingS = movingS;
    r.elapsedS = timerS;
    r.avgSpeedKmh = movingS ? (float)(distanceM / 1000.0 / (movingS / 3600.0))
                            : 0.0f;
    r.avgPowerW = powerCount ? (uint16_t)(powerSum / powerCount) : 0;
    r.normPowerW = npCount ? (uint16_t)pow(npSum4 / npCount, 0.25) : 0;
    r.avgHrBpm = hrCount ? (uint8_t)(hrSum / hrCount) : 0;
    r.climbedM = (float)climbedM;
    r.startUtc = startUtc;
    r.endUtc = endUtc ? endUtc : startUtc + timerS;
    r.tzMin = g_state.snapshot().tzMin;
    return r;
}

bool sdMounted() { return sdOk; }

int rideCount() {
    if (!sdOk) return 0;
    File dir = SD.open(RIDE_DIR);
    if (!dir) return 0;
    int n = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (!f.isDirectory()) n++;
        f.close();
    }
    dir.close();
    return n;
}

uint32_t sdFreeMB() {
    if (!sdOk) return 0;
    return (uint32_t)((SD.totalBytes() - SD.usedBytes()) / (1024ULL * 1024ULL));
}

void task(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(RECORD_INTERVAL_MS));
        if (!fit.isOpen()) continue;

        RideState s = g_state.snapshot();
        timerS++;
        accumulateStats(s);

        float grade = NAN;
        if (s.gpsFix) {
            if (havePrevFix) {
                double d = haversineM(lastLat, lastLon, s.latitude, s.longitude);
                // Reject jitter when stationary and jumps from bad fixes.
                if (d > 0.5 && d < 100.0) distanceM += d;
            }
            lastLat = s.latitude;
            lastLon = s.longitude;
            havePrevFix = true;
            grade = updateGrade(s.altitudeM);

            FitWriter::Record r;
            r.utc = s.utc;
            r.latitudeDeg = s.latitude;
            r.longitudeDeg = s.longitude;
            r.altitudeM = s.altitudeM;
            r.speedMs = s.speedKmh / 3.6f;
            r.distanceM = distanceM;
            r.powerW = s.powerW;
            r.heartRate = s.heartRateBpm;
            r.cadence = s.cadenceRpm;
            fit.writeRecord(r);
        }

        g_state.with([&](RideState& st) {
            st.distanceM = distanceM;
            st.elapsedS = timerS;
            st.movingS = movingS;
            st.climbedM = (float)climbedM;
            if (!isnan(grade)) {
                st.gradePercent = grade;
                st.gradeValid = true;
            }
        });

        if (timerS - lastFlushS >= FIT_FLUSH_EVERY_S) {
            fit.checkpoint();
            lastFlushS = timerS;
        }
    }
}

}  // namespace ride_recorder
