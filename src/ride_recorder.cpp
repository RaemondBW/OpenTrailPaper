#include "ride_recorder.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include "config.h"
#include "fit_writer.h"
#include "sd_bus.h"
#include "settings.h"
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
// millis() of the last position written. The phone path needs the real gap
// between points to derive speed; the device path is a steady 1 Hz.
uint32_t lastFixMs = 0;
// The phone fix already written, so a position that arrives every ~1 s is not
// re-stamped on every 1 s tick, and which source the log last reported.
uint32_t lastPhoneFixMs = 0;
bool phoneSourceLogged = false;
double distanceM = 0;
uint32_t timerS = 0;
uint32_t lastFlushS = 0;

// Auto-pause. timerS only counts while the bike is (apparently) moving;
// pausedS collects the rest, so wall-clock elapsed is always timerS + pausedS
// — that sum is what endUtc fallbacks and the FIT session's elapsed/timer
// split rely on. stoppedS is the run of consecutive stopped ticks that has to
// reach settings::autoPauseSec() before the timer actually freezes.
bool autoPaused = false;
uint32_t stoppedS = 0;
uint32_t pausedS = 0;
uint32_t pauseEntryS = 0;   // pausedS when this pause began, for the resume log
uint32_t lastMoveMs = 0;    // last tick with POSITIVE movement evidence

// Where the phone last said we were when the bike stopped — the anchor for
// motionEvidence's fallback witness. Seated by the pause state machine when
// movement ends, dropped the moment anything says we're moving, so each stop
// measures displacement from its own stopping point rather than a stale one.
double phoneAnchorLat = 0, phoneAnchorLon = 0;
float phoneAnchorAccM = 0;
bool phoneAnchorValid = false;

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
    // Named for the rider's wall clock, not UTC — an evening ride used to get
    // tomorrow's date in its name (UTC rolls over at 4-5 pm Pacific). Same
    // shifted-epoch trick as the diag log (diag.cpp localNow()): only the NAME
    // is local; the timestamps inside the FIT stay true UTC, which is what
    // every FIT consumer expects.
    time_t local = utc + (time_t)settings::tzMinutes() * 60;
    gmtime_r(&local, &tmv);
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
    autoPaused = false;
    stoppedS = 0;
    pausedS = 0;
    pauseEntryS = 0;
    lastMoveMs = 0;
    phoneAnchorValid = false;
    havePrevFix = false;
    lastFixMs = 0;
    lastPhoneFixMs = 0;
    phoneSourceLogged = false;
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

    // Climb from the barometer when one is fitted, else the map DEM elevation.
    // (The GPS chip's altitude is intentionally unused either way — far too
    // noisy.) 3 m hysteresis smooths the accumulation; the barometer is smooth
    // enough not to need it, but keeping one path means ascent totals do not
    // change character depending on what is plugged in.
    if (s.baroValid || s.mapElevationValid) {
        float elev = s.baroValid ? s.baroAltM : s.mapElevationM;
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

// Is the bike moving? Judged from the most direct evidence available, sensors
// before GPS: turning pedals (power or cadence non-zero) or a turning wheel is
// movement no matter what the position fix thinks — a trainer ride must never
// pause — while GPS speed is the witness of last resort, catching the coasting
// descent where pedals and cranks are all still.
//
// Three answers, not two. MOVING and STOPPED are positive testimony: a sensor
// only votes while its data is fresh (a sleeping power meter abstains rather
// than testifying "0 W"), and the wheel counter can only ever vote "moving" (a
// silent one is indistinguishable from an absent one). BLIND is no witnesses
// at all — no fix, every sensor asleep — and the CALLER decides what that
// means, because it depends on history this function doesn't have: blind
// moments after real movement is a tunnel and the timer should run; blind
// with no movement in living memory is a desk, a trainer-less garage, or a
// bike carried into a cafe, and "no evidence of moving" should not keep the
// timer counting forever. (The first version returned moving-when-blind
// unconditionally, and a ride recorded indoors with no fix never paused at
// all — and a fix LOST while paused would have resumed the timer.)
//
// `resuming` raises the GPS bar from rolling speed to distinctly-riding speed:
// a stationary fix wobbles, and a wobble that un-pauses the ride would need
// another full stopped streak to re-pause it.
enum class Motion { MOVING, STOPPED, BLIND };

Motion motionEvidence(const RideState& s, bool resuming) {
    const uint32_t now = millis();
    auto fresh = [&](uint32_t ms) { return ms != 0 && now - ms < 5000; };
    int witnesses = 0;
    bool moving = false;
    if (fresh(s.powerMs) && s.powerW != 0xFFFF) {
        witnesses++;
        if (s.powerW > 0) moving = true;
    }
    if (fresh(s.cadenceMs) && s.cadenceRpm != 0xFF) {
        witnesses++;
        if (s.cadenceRpm > 0) moving = true;
    }
    if (fresh(s.wheelMoveMs)) moving = true;   // rev counter advanced recently
    if (s.gpsFix) {
        witnesses++;
        if (s.speedKmh > (resuming ? 5.0f : 3.0f)) moving = true;
    } else if (s.phoneFixValid && now - s.phoneFixMs < 10000 &&
               s.phoneAccM > 0 && s.phoneAccM <= 50.0f) {
        // No fix of our own: the PHONE's location is the witness of last
        // resort — same accuracy gate as the record stream's fallback. It has
        // no speed, so movement is displacement from the anchor seated where
        // the bike stopped: farther than the combined accuracy circles (floor
        // 30 m) is a rider leaving, not a fix wandering. This is what lets a
        // ride parked INDOORS — device GPS dead, phone still located — resume
        // as the rider pulls away instead of waiting out a fresh device fix.
        witnesses++;
        if (phoneAnchorValid) {
            const float leash = fmaxf(30.0f, phoneAnchorAccM + s.phoneAccM);
            if (haversineM(phoneAnchorLat, phoneAnchorLon,
                           s.phoneLat, s.phoneLon) > leash) moving = true;
        }
        // Anchor not seated yet: this fix still counts as a witness (it can
        // testify "stopped"), and the state machine seats the anchor from it.
    }
    if (moving) return Motion::MOVING;
    return witnesses ? Motion::STOPPED : Motion::BLIND;
}

// How long a blind spell keeps the benefit of the doubt after the last
// positive movement. Long enough to coast through any ordinary tunnel or a
// minute of dense tree cover without the timer flinching; short enough that a
// bike carried indoors stops accruing ride time within a couple of minutes.
// Riders with a power meter, cadence or wheel sensor are barely affected —
// their sensors keep testifying where GPS goes deaf.
constexpr uint32_t BLIND_GRACE_MS = 120 * 1000;

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
// Shove the card back to a known state before asking the library to mount it.
//
// WHY. This device cannot power-cycle its SD card — there is no gate on that
// rail (the XL9555 gates GPS/LoRa only). So when the firmware resets in the
// MIDDLE of an SD transaction, which is exactly what a watchdog reset during
// spiTransferBytesNL() does, the card keeps the state it was left in: mid
// command, and with CRC checking still enabled from the previous session. The
// SD spec only allows CRC on for CMD0 and CMD8, so the mount sequence's first
// command comes back a CRC error and the whole mount aborts — on a card that is
// perfectly healthy and mounts fine in a reader. That is espressif/esp-idf
// #14000; the tolerant behaviour landed in IDF 5.x and this build is on 4.4.
//
// The sequence is the one the SD physical-layer spec prescribes for entering
// SPI mode, and it is what a power cycle would otherwise have done for us:
//   * >= 74 clocks with CS and MOSI HIGH, to let the card's internal state
//     machine finish whatever it was doing (80 here, ten 0xFF bytes);
//   * then CMD0 GO_IDLE_STATE, which carries a FIXED, always-valid CRC (0x95)
//     and so is the one command a card still in CRC mode will accept.
//
// Cheap (a few hundred microseconds at 400 kHz) and harmless on a card that is
// already idle, so it runs before every attempt rather than only after a crash.
void sdSpiForceIdle() {
    SPI.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    digitalWrite(BOARD_SD_CS, HIGH);
    for (int i = 0; i < 10; ++i) SPI.transfer(0xFF);   // >= 74 clocks, CS high

    digitalWrite(BOARD_SD_CS, LOW);
    SPI.transfer(0xFF);
    static const uint8_t kCmd0[] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x95};
    for (uint8_t b : kCmd0) SPI.transfer(b);
    for (int i = 0; i < 10; ++i) {                     // R1, bit7 clear
        if ((SPI.transfer(0xFF) & 0x80) == 0) break;
    }
    digitalWrite(BOARD_SD_CS, HIGH);
    SPI.transfer(0xFF);                                // release the bus
    SPI.endTransaction();
}

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
        sdSpiForceIdle();                    // clear a garbled prior transaction
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
        st.ridePaused = false;
        st.pausedForS = 0;
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
    // Wall-clock fallback is timer PLUS paused time — the timer alone stopped
    // tracking the clock the first time the ride auto-paused.
    endUtc = s.timeValid ? s.utc : startUtc + timerS + pausedS;
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
        diag::log("ride saved: %.2f km, %lu s (+%lu s paused)", distanceM / 1000.0,
                  (unsigned long)timerS, (unsigned long)pausedS);
        Serial.printf("[rec] ride saved: %.1f km, %lu s (+%lu s paused)\n",
                      distanceM / 1000.0, (unsigned long)timerS,
                      (unsigned long)pausedS);
    }
    // Ride is over — clear the trip so the dashboard reads zero, ready for the
    // next ride (the summary was already captured before stopping).
    resetStats();
    g_state.with([](RideState& st) {
        st.recording = false;
        st.ridePaused = false;
        st.pausedForS = 0;
        st.distanceM = 0;
        st.elapsedS = 0;
        st.movingS = 0;
        st.climbedM = 0;
        st.gradeValid = false;
    });
}

bool isRecording() { return rideActive; }

bool longAutoPaused() {
    // Cross-task reads of the recorder's own counters; a tick of staleness is
    // fine for a power-policy answer. 2 min: longer than any stoplight, short
    // enough that a real stop starts saving within a coffee's first sips.
    return rideActive && autoPaused && (pausedS - pauseEntryS) > 120;
}

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
    r.endUtc = endUtc ? endUtc : startUtc + timerS + pausedS;
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

        // Auto-pause: freeze the timer once the bike has been demonstrably
        // stopped for the configured run of seconds; resume the moment anything
        // says it is moving again. Paused ticks write no records — the
        // timestamp gap in the record stream is how a FIT reader sees the stop.
        // Deliberately NOT a mid-file timer-stop/start event pair, which is
        // what the FIT spec would prefer: repair() walks the record stream at a
        // fixed byte stride and treats the first non-record byte as the torn
        // end of the file, so an event written after a crash-recovered pause
        // would silently amputate the rest of the ride.
        const int pauseAfterS = settings::autoPauseSec();
        if (pauseAfterS > 0) {
            const Motion m = motionEvidence(s, /*resuming=*/autoPaused);
            if (m == Motion::MOVING) lastMoveMs = millis() | 1;
            // BLIND inherits the last positive answer for a grace window — a
            // tunnel keeps its timer — but only while RUNNING. A paused ride
            // needs positive movement to resume: losing the fix while parked
            // at a cafe is not evidence of riding. Blind with no movement on
            // record (a ride started indoors, before first fix) counts as
            // stopped from the first tick.
            const bool moving =
                m == Motion::MOVING ||
                (m == Motion::BLIND && !autoPaused && lastMoveMs &&
                 millis() - lastMoveMs < BLIND_GRACE_MS);

            // Phone-anchor upkeep for motionEvidence's fallback witness: while
            // stopped, the anchor marks the stopping point; any movement drops
            // it so the NEXT stop seats a fresh one where it actually happens.
            if (moving) {
                phoneAnchorValid = false;
            } else if (!phoneAnchorValid && s.phoneFixValid &&
                       millis() - s.phoneFixMs < 10000 &&
                       s.phoneAccM > 0 && s.phoneAccM <= 50.0f) {
                phoneAnchorLat = s.phoneLat;
                phoneAnchorLon = s.phoneLon;
                phoneAnchorAccM = s.phoneAccM;
                phoneAnchorValid = true;
            }
            if (autoPaused) {
                if (moving) {
                    autoPaused = false;
                    stoppedS = 0;
                    diag::log("rec: auto-resume (paused %lus)",
                              (unsigned long)(pausedS - pauseEntryS));
                }
            } else if (!moving) {
                if ((int)++stoppedS >= pauseAfterS) {
                    autoPaused = true;
                    pauseEntryS = pausedS;
                    diag::log("rec: auto-pause (%d s stopped) at %.2f km",
                              pauseAfterS, distanceM / 1000.0);
                    // A stop is a natural safe point — persist what we have, so
                    // a mid-stop power loss costs nothing.
                    sdLock();
                    fit.checkpoint();
                    sdUnlock();
                }
            } else {
                stoppedS = 0;
            }
        } else if (autoPaused) {
            autoPaused = false;   // setting switched off mid-pause
        }

        if (autoPaused) {
            pausedS++;
            g_state.with([&](RideState& st) {
                st.ridePaused = true;
                st.pausedForS = pausedS - pauseEntryS;   // drives the banner's counter
            });
            continue;
        }

        timerS++;
        accumulateStats(s);

        // Position for this record. The device's own receiver first; the phone's
        // location only while we have no fix of our own.
        //
        // The phone is a fallback, not a peer, and it is gated hard: its fix has
        // to be recent, it has to have told us an accuracy good enough to be
        // track data (a cell/wifi fix is hundreds of metres and would draw a
        // ride through the middle of blocks), and each one is written ONCE. The
        // app only sends every ~3 s, so writing the same coordinates on every 1 s
        // tick would stamp a stationary rider into the file three times over and
        // then jump.
        //
        // Without this the file simply had a hole: no fix meant no record and no
        // distance, so the opening minutes of a ride — exactly when the receiver
        // is coldest and the phone is right there in a pocket — went unrecorded.
        constexpr uint32_t kPhoneFixMaxAgeMs = 10000;
        constexpr float kPhoneFixMaxAccM = 50.0f;

        const bool phoneUsable = s.phoneFixValid &&
                                 millis() - s.phoneFixMs < kPhoneFixMaxAgeMs &&
                                 s.phoneAccM > 0 && s.phoneAccM <= kPhoneFixMaxAccM;
        // Which source the ride is running on — NOT whether this particular tick
        // writes a point. Between the phone's ~1 s updates there is nothing new
        // to write, and logging off that would flap the source line every tick.
        const bool phoneMode = !s.gpsFix && phoneUsable;
        const bool usePhone = phoneMode && s.phoneFixMs != lastPhoneFixMs;

        if (phoneMode != phoneSourceLogged) {
            phoneSourceLogged = phoneMode;
            diag::log("rec: position source -> %s", phoneMode
                          ? "PHONE (own receiver has no fix)" : "device GPS");
        }

        float grade = NAN;
        if (s.gpsFix || usePhone) {
            const double lat = s.gpsFix ? s.latitude : s.phoneLat;
            const double lon = s.gpsFix ? s.longitude : s.phoneLon;
            // Seconds since the last recorded point. The phone path needs the
            // real gap: its updates are ~1 s while recording but 3 s otherwise,
            // and iOS can deliver later still, so assuming the tick interval
            // would put both the speed and the jitter window out.
            const uint32_t nowMs = millis();
            const float dtSec = havePrevFix && lastFixMs
                                    ? (nowMs - lastFixMs) / 1000.0f
                                    : (RECORD_INTERVAL_MS / 1000.0f);
            float phoneSpeedMs = NAN;

            if (havePrevFix) {
                double d = haversineM(lastLat, lastLon, lat, lon);
                if (s.gpsFix) {
                    // Reject jitter when stationary and jumps from bad fixes.
                    if (d > 0.5 && d < 100.0) distanceM += d;
                } else if (d < 0.5) {
                    // Below the jitter floor: the rider is stopped. Say zero —
                    // leaving it unknown would let a reader interpolate a speed
                    // across the stop.
                    phoneSpeedMs = 0.0f;
                } else if (d < 40.0 * dtSec) {
                    // Same jitter idea as the device path, scaled to the real
                    // gap: 40 m/s is 144 km/h, so anything past it is a fix
                    // jumping, not a rider. No distance and no speed claim.
                    distanceM += d;
                    if (dtSec > 0.1f) phoneSpeedMs = (float)(d / dtSec);
                }
            }
            lastLat = lat;
            lastLon = lon;
            lastFixMs = nowMs;
            havePrevFix = true;
            if (usePhone) lastPhoneFixMs = s.phoneFixMs;
            if (s.baroValid) grade = updateGrade(s.baroAltM);
            else if (s.mapElevationValid) grade = updateGrade(s.mapElevationM);

            FitWriter::Record r;
            // With no lock of our own, s.utc is whatever the last fix left
            // behind. The phone sent its own clock with the position; prefer it.
            r.utc = s.gpsFix ? s.utc
                             : (s.phoneUtc ? s.phoneUtc : (uint32_t)time(nullptr));
            r.latitudeDeg = lat;
            r.longitudeDeg = lon;
            // Record the map DEM elevation so the ride profile is accurate.
            // The raw GPS altitude is deliberately NOT used as a fallback: it's
            // far too noisy (the summary's ascent ignores it for the same
            // reason), and mixing it into the record stream with DEM values
            // makes phone-side ascent totals disagree with the device. Mark the
            // point's altitude invalid instead when the DEM has no value.
            // A barometer beats the elevation grid for a ride profile: the DEM
            // is quantised to its own grid and knows nothing about a bridge or
            // an overpass, while pressure resolves a metre and follows the road
            // the rider is actually on. The DEM stays the fallback, and stays
            // the thing that CALIBRATES the barometer (aux_sensors).
            r.altitudeM = s.baroValid ? s.baroAltM
                        : s.mapElevationValid ? s.mapElevationM : NAN;
            // s.speedKmh comes from the receiver, so on the phone path it is
            // whatever the last device fix left behind — stale, and usually the
            // speed the rider was doing before the signal went. Derive it from
            // the ground actually covered instead.
            r.speedMs = s.gpsFix ? s.speedKmh / 3.6f : phoneSpeedMs;
            r.distanceM = distanceM;
            r.powerW = s.powerW;
            r.heartRate = s.heartRateBpm;
            r.cadence = s.cadenceRpm;
            sdLock();
            fit.writeRecord(r);
            sdUnlock();
        }

        g_state.with([&](RideState& st) {
            st.ridePaused = false;
            st.pausedForS = 0;
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
