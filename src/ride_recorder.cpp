#include "ride_recorder.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include "config.h"
#include "fit_writer.h"
#include "sd_bus.h"
#include "usb_storage.h"
#include "diag.h"

namespace {

FitWriter fit;
bool sdOk = false;
// Set when a mount succeeds AFTER boot gave up. Everything gated on the boot
// mount (routes, the map index, USB mass storage) was skipped in that case and
// has to be re-run, or the card sits mounted and completely unused.
bool lateMount = false;
char ridePath[48];

// The rider started a ride and hasn't stopped it. Deliberately NOT derived from
// fit.isOpen(): the FIT handle lives on an SPI bus shared with the map store,
// the diag log and USB mass storage, and a ride must outlive any trouble down
// there. Only startRide()/stopRide() move this.
bool rideActive = false;
bool loggedFileLost = false;   // so a lost handle logs once, not once a second

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

// A reset mid-ride (watchdog, brownout, flat battery) leaves the ride on the
// card without the lap/session/activity tail and CRC that make it an activity,
// so Strava and friends reject it — the ride looks lost even though every
// record is sitting there. Put those files back together at boot, before the
// UI or a USB host can touch the card.
//
// IMPORTANT: gather the filenames with the directory open, then CLOSE it before
// opening any file to repair. Opening a file (repair) while a directory handle
// is still iterating is a known way to wedge / corrupt the ESP32 SD (FatFs)
// stack — which showed up as "the device can't recognize the SD card".
void recoverInterruptedRides() {
    constexpr int MAX_SCAN = 32;
    char names[MAX_SCAN][40];
    int nameCount = 0;

    sdLock();
    File dir = SD.open(RIDE_DIR);
    if (dir) {
        for (File f = dir.openNextFile(); f && nameCount < MAX_SCAN;
             f = dir.openNextFile()) {
            if (!f.isDirectory()) {
                const char* nm = f.name();
                const char* base = strrchr(nm, '/');
                base = base ? base + 1 : nm;
                // Skip our own quarantine markers — they are not rides, and
                // repairing one would only breed a ".bad.bad".
                size_t len = strlen(base);
                if (len >= 4 && strcmp(base + len - 4, ".bad") == 0) {
                    f.close();
                    continue;
                }
                strncpy(names[nameCount], base, sizeof(names[0]) - 1);
                names[nameCount][sizeof(names[0]) - 1] = 0;
                nameCount++;
            }
            f.close();
        }
        dir.close();   // directory fully closed before any repair opens a file
    }

    char toDelete[8][48];
    int deleteCount = 0;
    int repaired = 0;
    for (int i = 0; i < nameCount; ++i) {
        char path[48];
        snprintf(path, sizeof(path), RIDE_DIR "/%.39s", names[i]);

        // Quarantine before touching the file, clear it after. If we never get
        // to the clear — watchdog, panic, power cut — the marker survives on the
        // card and the next boot skips this file instead of dying on it again.
        // Without this, one unrepairable ride is a permanent brick that only a
        // card reader can undo.
        char mark[56];
        snprintf(mark, sizeof(mark), "%s.bad", path);
        if (SD.exists(mark)) {
            diag::log("ride skipped (previous recovery did not survive it): %s",
                      path);
            continue;
        }
        { File m = SD.open(mark, FILE_WRITE); if (m) m.close(); }

        FitWriter::RepairResult r = FitWriter::repair(SD, path);
        SD.remove(mark);   // survived — this file is not the problem
        if (r.status == FitWriter::RepairResult::REPAIRED) {
            repaired++;
            diag::log("ride recovered: %s — %.2f km, %lu s (%d pts)", path,
                      r.distanceM / 1000.0, (unsigned long)r.elapsedS, r.records);
        } else if (r.status == FitWriter::RepairResult::EMPTY) {
            // Died before the first GPS fix: a header and nothing else.
            if (deleteCount < 8) {
                snprintf(toDelete[deleteCount++], sizeof(toDelete[0]), "%s", path);
            }
        }
    }
    for (int i = 0; i < deleteCount; ++i) SD.remove(toDelete[i]);
    sdUnlock();

    if (repaired) {
        Serial.printf("[rec] recovered %d interrupted ride(s)\n", repaired);
    }
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

    // Climb from the map DEM elevation (the GPS chip's altitude is intentionally
    // unused — it's far too noisy). 3 m hysteresis smooths the accumulation.
    if (s.mapElevationValid) {
        float elev = s.mapElevationM;
        if (!climbBaseValid) {
            climbBaseAlt = elev;
            climbBaseValid = true;
        } else if (elev > climbBaseAlt + 3.0f) {
            climbedM += elev - climbBaseAlt;
            climbBaseAlt = elev;
        } else if (elev < climbBaseAlt - 3.0f) {
            climbBaseAlt = elev;
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

namespace {

// Mount the card. Caller holds sdLock.
//
// Retry: the SPI card-init handshake is flaky right after power-on and can fail
// the first time or two, especially if a prior session left the card
// mid-transaction (only a clean re-init clears it). Stay at the library's proven
// 4 MHz — the same clock the recorder has always used — and just give it a few
// gentle attempts with a settle delay, dropping to 1 MHz last-ditch. (Do NOT
// start high: a too-fast probe can wedge a marginal card so the slower retries
// then also fail.)
//
// `logFailures` is off for the background retry, which would otherwise write
// three lines every 30 s for as long as a card is simply absent.
bool mountLocked(bool logFailures) {
    // settleMs is waited BEFORE the attempt. The old schedule (three tries, 50 ms
    // apart) gave the card ~100 ms to come up and then declared it absent, which
    // on this board is simply too early: measured on a cold boot with a known-good
    // 32 GB SDHC, every probe inside the first ~1.5 s failed and the mount only
    // took at ~2 s (investigations/sd-boot-mount-2026-08-05.log). Boot therefore
    // ALWAYS missed the card and left the device in its "no SD" state until the
    // 30 s background retry rescued it — by which point map/route loading had
    // already been skipped for the session.
    //
    // The cost is paid only on failure: a card that answers immediately still
    // mounts on the first attempt with zero added delay, and a genuinely absent
    // card costs ~2.3 s of settle on top of SD.begin()'s own timeouts. Boot is
    // never blocked on the result — a failed mount just disables recording.
    static const struct { uint32_t settleMs; uint32_t freq; } kAttempts[] = {
        {0,    4000000},
        {100,  4000000},
        {400,  4000000},
        {800,  4000000},
        {1000, 1000000},   // last-ditch, slow clock for a marginal card
    };
    for (auto& a : kAttempts) {
        uint32_t f = a.freq;
        if (a.settleMs) delay(a.settleMs);   // let the card/bus settle first
        // max_files: the library default is 5 open files for the WHOLE firmware,
        // and a ride holds one of them open from start to finish. Ten leaves room
        // for a map tile, a route, a diag flush and a BLE download at the same
        // time without opens starting to fail mid-ride.
        if (SD.begin(BOARD_SD_CS, SPI, f, "/sd", 10)) return true;
        if (logFailures) {
            // Deliberately NOT logging cardType()/cardSize() here. A previous
            // version did, to tell "card not talking" from "bad filesystem", but
            // that reading can never happen: SD.begin() unmounts, uninits and
            // sets _pdrv = 0xFF itself before returning false, and both accessors
            // early-return on that sentinel. They report NONE/0 unconditionally,
            // so the line only ever looked like a dead card. (Arduino-esp32
            // SD.cpp: begin() 38-42, cardType() 62, cardSize() 70.)
            diag::log("[rec] SD.begin failed @%uMHz (settle %ums)",
                      (unsigned)(f / 1000000), (unsigned)a.settleMs);
        }
        SD.end();
    }
    return false;
}

}  // namespace

bool begin() {
    // SD and LoRa share the SPI bus; a floating LoRa CS corrupts SD traffic.
    pinMode(BOARD_LORA_CS, OUTPUT);
    digitalWrite(BOARD_LORA_CS, HIGH);
    pinMode(BOARD_SD_CS, OUTPUT);
    digitalWrite(BOARD_SD_CS, HIGH);

    SPI.begin(BOARD_SPI_SCLK, BOARD_SPI_MISO, BOARD_SPI_MOSI);
    sdLock();
    sdOk = mountLocked(true);
    if (sdOk && !SD.exists(RIDE_DIR)) SD.mkdir(RIDE_DIR);
    sdUnlock();
    if (!sdOk) {
        Serial.println("[rec] SD mount failed — recording disabled");
        return false;
    }
    Serial.printf("[rec] SD ready, %llu MB free\n",
                  (SD.totalBytes() - SD.usedBytes()) / (1024ULL * 1024ULL));
    // Recovery deliberately does NOT run here. It used to, and a card carrying
    // interrupted rides could then take longer than the interrupt watchdog
    // allows — the device reset mid-recovery, which tore one more file, and the
    // next boot found strictly more work than the last. That is an unbreakable
    // boot loop, and it is unbreakable from the device: the only escape was
    // pulling the card. setup() must not contain unbounded card work. The UI
    // task calls recoverRides() once it is running, still before usb_storage
    // hands the card to a host.
    return true;
}

void startRide() {
    if (!sdOk || rideActive || usb_storage::hostActive()) return;

    RideState s = g_state.snapshot();
    if (!s.timeValid) {
        Serial.println("[rec] no GPS time yet — can't start ride");
        return;
    }

    makeRidePath(ridePath, sizeof(ridePath), s.utc);
    sdLock();
    bool opened = fit.begin(SD, ridePath, s.utc);
    sdUnlock();
    if (!opened) {
        // The mount was only ever established at boot, so a card that dropped
        // since then failed every ride for the rest of the session. One remount
        // and one retry — cheap, and the difference between losing a ride and
        // not. (rideActive is still false here, so remount() will proceed.)
        diag::log("[rec] ride file open failed — remounting");
        if (remount("ride file open failed")) {
            sdLock();
            opened = fit.begin(SD, ridePath, s.utc);
            sdUnlock();
        }
    }
    if (!opened) {
        Serial.printf("[rec] failed to open %s\n", ridePath);
        diag::log("[rec] ride NOT started: cannot open %s", ridePath);
        return;
    }

    startUtc = s.utc;
    resetStats();
    rideActive = true;
    loggedFileLost = false;
    g_state.with([](RideState& st) {
        st.recording = true;
        st.distanceM = 0;
        st.elapsedS = 0;
        st.movingS = 0;
        st.climbedM = 0;
        st.gradeValid = false;
    });
    diag::log("ride start -> %s", ridePath);
}

void stopRide(bool save) {
    if (!rideActive) return;
    rideActive = false;
    RideState s = g_state.snapshot();
    endUtc = s.timeValid ? s.utc : startUtc + timerS;
    RideSummary sm = summary();   // fold the roll-up into the FIT session message
    sdLock();
    fit.finish(endUtc, distanceM, timerS,
               {movingS, sm.avgSpeedKmh, sm.avgPowerW, sm.normPowerW,
                sm.avgHrBpm, (float)climbedM});
    if (!save) SD.remove(ridePath);
    sdUnlock();
    if (!save) {
        diag::log("ride discarded");
    } else {
        diag::log("ride saved: %.2f km, %lu s", distanceM / 1000.0,
                  (unsigned long)timerS);
        Serial.printf("[rec] ride saved: %.1f km, %lu s\n", distanceM / 1000.0,
                      (unsigned long)timerS);
    }
    // Ride is over — clear the trip so the dashboard reads zero, ready for the
    // next ride (the summary was already captured before stopping).
    resetStats();
    g_state.with([](RideState& st) {
        st.recording = false;
        st.distanceM = 0;
        st.elapsedS = 0;
        st.movingS = 0;
        st.climbedM = 0;
        st.gradeValid = false;
    });
}

bool isRecording() { return rideActive; }

const char* currentRideFile() {
    if (!fit.isOpen()) return "";
    const char* base = strrchr(ridePath, '/');
    return base ? base + 1 : ridePath;
}

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
    r.useMiles = g_state.snapshot().useMiles;
    return r;
}

bool remount(const char* why) {
    // Never mid-ride: SD.end() with the FIT file open leaves the card
    // mid-transaction, which is the failure that makes it refuse CMD0 on every
    // subsequent mount. Callers hold their request and retry once the ride ends.
    if (rideActive) return sdOk;
    sdLock();
    SD.end();
    sdOk = mountLocked(true);
    if (sdOk && !SD.exists(RIDE_DIR)) SD.mkdir(RIDE_DIR);
    sdUnlock();
    diag::log("sd: remount (%s) -> %s", why, sdOk ? "ok" : "FAILED");
    return sdOk;
}

void retryMountIfNeeded() {
    // Nothing to do when it's mounted, and nothing we're allowed to do while a
    // host computer owns the card over USB.
    if (sdOk || rideActive || usb_storage::hostActive()) return;

    static uint32_t lastTry = 0;
    static uint16_t tries = 0;
    uint32_t now = millis();
    if (lastTry != 0 && now - lastTry < 30000) return;
    lastTry = now;

    sdLock();
    bool ok = mountLocked(tries < 3);   // stop logging once it's clearly absent
    if (ok && !SD.exists(RIDE_DIR)) SD.mkdir(RIDE_DIR);
    sdUnlock();

    if (ok) {
        sdOk = true;
        lateMount = true;   // boot ran without a card — see consumeLateMount()
        diag::log("sd: card back after %u retr%s", (unsigned)tries + 1,
                  tries ? "ies" : "y");
        tries = 0;
    } else {
        if (tries == 3) diag::log("sd: still absent — retrying quietly from here");
        if (tries < 0xFFFF) tries++;
    }
}

// True once, on the first call after a card mounted later than boot. The caller
// is expected to run the SD-dependent bring-up that setup() skipped. Consumed
// (not just read) so the work happens exactly once per late mount.
bool consumeLateMount() {
    if (!lateMount) return false;
    lateMount = false;
    return true;
}

// Put interrupted rides back together. Called from the UI task, NOT from
// setup() — see the note in begin(). Safe to call more than once; a ride that
// is already finished is left alone.
void recoverRides() {
    if (!sdOk) return;
    recoverInterruptedRides();
}

// SD is "unavailable" to the firmware while a host computer owns it over USB.
bool sdMounted() { return sdOk && !usb_storage::hostActive(); }

int rideCount() {
    if (!sdOk) return 0;
    sdLock();
    File dir = SD.open(RIDE_DIR);
    int n = 0;
    if (dir) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            if (!f.isDirectory()) n++;
            f.close();
        }
        dir.close();
    }
    sdUnlock();
    return n;
}

uint32_t sdFreeMB() {
    if (!sdOk) return 0;
    sdLock();
    uint32_t mb = (uint32_t)((SD.totalBytes() - SD.usedBytes()) / (1024ULL * 1024ULL));
    sdUnlock();
    return mb;
}

void task(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(RECORD_INTERVAL_MS));
        // Never write while a host computer owns the SD over USB.
        if (!rideActive || usb_storage::hostActive()) continue;

        // The FIT handle went away underneath us (SD trouble — the card is
        // shared with the map store, diag logging and USB storage). Keep the
        // ride running: the timer, distance and summary stay live, auto-sleep
        // stays blocked, and the rider stops the ride when they mean to. Say so
        // in the log, once, so a short FIT is diagnosable after the fact.
        if (!fit.isOpen() && !loggedFileLost) {
            loggedFileLost = true;
            diag::log("rec: FIT handle lost mid-ride (%s) — ride still timing, "
                      "records not being written", ridePath);
        }

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
            if (s.mapElevationValid) grade = updateGrade(s.mapElevationM);

            FitWriter::Record r;
            r.utc = s.utc;
            r.latitudeDeg = s.latitude;
            r.longitudeDeg = s.longitude;
            // Record the map DEM elevation so the ride profile is accurate.
            // The raw GPS altitude is deliberately NOT used as a fallback: it's
            // far too noisy (the summary's ascent ignores it for the same
            // reason), and mixing it into the record stream with DEM values
            // makes phone-side ascent totals disagree with the device. Mark the
            // point's altitude invalid instead when the DEM has no value.
            r.altitudeM = s.mapElevationValid ? s.mapElevationM : NAN;
            r.speedMs = s.speedKmh / 3.6f;
            r.distanceM = distanceM;
            r.powerW = s.powerW;
            r.heartRate = s.heartRateBpm;
            r.cadence = s.cadenceRpm;
            sdLock();
            fit.writeRecord(r);
            sdUnlock();
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
            sdLock();
            fit.checkpoint();
            sdUnlock();
            lastFlushS = timerS;
        }
    }
}

}  // namespace ride_recorder
