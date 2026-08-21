#include "diag.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <time.h>

#include "ride_recorder.h"
#include "sd_bus.h"
#include "settings.h"

namespace {

char* buf = nullptr;
size_t len = 0;
constexpr size_t CAP = 48 * 1024;         // in-RAM staging before an SD flush
SemaphoreHandle_t mtx = nullptr;
constexpr char LOG_DIR[] = "/logs";
char activePath[48] = "/logs/pending.log";

// The rider's wall clock, as a shifted epoch fed to gmtime_r. The system clock
// stays UTC (GPS writes it, the FIT file and the RTC depend on that); only the
// log's presentation is local. settings::tzMinutes() is a plain static with a
// sane default, so this is safe even for lines logged before settings::begin().
time_t localNow() {
    return time(nullptr) + (time_t)settings::tzMinutes() * 60;
}

// One log file per LOCAL day: /logs/YYYYMMDD.log — small and easy to grab.
// Local, not UTC: an evening ride in California used to land in tomorrow's
// file (UTC rolls over at 4-5 pm Pacific), so "yesterday's ride" meant knowing
// which side of the boundary you rode on. The line timestamps below use the
// same clock, so the file named for a day contains times from that day.
// Before the clock is set (no GPS fix yet) lines go to /logs/pending.log.
void computeLogPath(char* out, size_t n) {
    time_t now = localNow();
    if (now > 1735689600) {
        struct tm t;
        gmtime_r(&now, &t);
        snprintf(out, n, "%s/%04d%02d%02d.log", LOG_DIR, t.tm_year + 1900,
                 t.tm_mon + 1, t.tm_mday);
    } else {
        snprintf(out, n, "%s/pending.log", LOG_DIR);
    }
}

void timestamp(char* out, size_t n) {
    time_t now = localNow();
    if (now > 1735689600) {               // system clock is set (GPS synced)
        struct tm t;
        gmtime_r(&now, &t);
        snprintf(out, n, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        snprintf(out, n, "+%lus", (unsigned long)(millis() / 1000));
    }
}

}  // namespace

namespace diag {

void begin() {
    buf = (char*)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
    len = 0;
    mtx = xSemaphoreCreateMutex();
}

void log(const char* fmt, ...) {
    char line[220];
    char ts[16];
    timestamp(ts, sizeof(ts));
    int pre = snprintf(line, sizeof(line), "[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + pre, sizeof(line) - pre - 2, fmt, ap);
    va_end(ap);
    size_t n = strlen(line);
    line[n++] = '\n';
    line[n] = 0;
    Serial.print(line);

    if (!buf || !mtx) return;
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (len + n > CAP) {                   // drop the oldest half to make room
        size_t drop = len / 2;
        memmove(buf, buf + drop, len - drop);
        len -= drop;
    }
    if (len + n <= CAP) { memcpy(buf + len, line, n); len += n; }
    xSemaphoreGive(mtx);
}

void flushToSD() {
    if (!buf || !mtx) return;
    if (!ride_recorder::sdMounted()) return;
    // Nothing to write, nothing to lock. The BLE task calls this at 1 Hz, and
    // without the check every call was a pointless sdLock/sdUnlock round trip.
    // (Unlocked read of `len`: a line landing right now waits for the next call.)
    if (len == 0) return;
    // Recording normally defers the flush so the log never contends with the
    // 1 Hz FIT writes — but never at the price of the log itself. When the ring
    // passes half full, flush anyway: unflushed lines are evicted oldest-half
    // first, which on the 2026-08-16 ride deleted ninety minutes of battery
    // telemetry. One append per several minutes under the same sdLock the
    // recorder already takes is noise; it also means a mid-ride power loss
    // keeps the log up to the last high-water flush instead of losing it all.
    // (`len` is read unlocked: a stale read moves one flush by a line or two.)
    if (ride_recorder::isRecording() && len < CAP / 2) return;
    // sdLock BEFORE the diag mutex — the same order as every task that calls
    // diag::log() while holding the SD lock (recovery, map/tile saves, ...).
    // The old order here (mtx first, then sdLock) deadlocked against exactly
    // those callers: this task held the log mutex waiting for the card while
    // they held the card waiting for the log mutex. Card-present boots only,
    // since without a mount this function returns before locking anything.
    sdLock();
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (len == 0) { xSemaphoreGive(mtx); sdUnlock(); return; }
    if (!SD.exists(LOG_DIR)) SD.mkdir(LOG_DIR);
    computeLogPath(activePath, sizeof(activePath));   // today's file (UTC)
    File f = SD.open(activePath, FILE_APPEND);
    if (f) {
        f.write((const uint8_t*)buf, len);
        f.close();
        len = 0;
    }
    xSemaphoreGive(mtx);
    sdUnlock();
}

const char* logPath() {
    computeLogPath(activePath, sizeof(activePath));
    return activePath;
}

}  // namespace diag
