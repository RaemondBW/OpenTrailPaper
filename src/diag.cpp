#include "diag.h"
#include "crash_report.h"
#include "memfault_service.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <time.h>
#include <Preferences.h>
#include "diag_buffer.h"

#include "ride_recorder.h"
#include "sd_bus.h"
#include "settings.h"

namespace {

struct StorageTrace { uint32_t magic, value, inverted, uptime; };
RTC_NOINIT_ATTR volatile StorageTrace storageTrace;
constexpr uint32_t TRACE_MAGIC = 0x53445031;
char* buf = nullptr;
DiagBuffer pending(nullptr, 0);
constexpr size_t BOOT_CAP = 8192;
constexpr size_t SNAPSHOT_CAP = 3072;
char* boot = nullptr;
size_t bootLen = 0;
bool bootFinished = false;
bool bootReplayDone = false;
char previous[SNAPSHOT_CAP + 96] = {};
size_t previousLen = 0;
bool checkpointSaved = false;
uint32_t retryAfterMs = 0;
uint32_t lastFlushMs = 0;
bool retryWaiting = false;
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

void storageStage(uint8_t stage, uint8_t attempt, uint8_t r1) {
    storageTrace.magic = 0;
    storageTrace.value = ((uint32_t)stage << 16) | ((uint32_t)attempt << 8) | r1;
    storageTrace.inverted = ~storageTrace.value;
    storageTrace.uptime = millis();
    storageTrace.magic = TRACE_MAGIC;
}

void begin() {
    buf = (char*)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
    size_t capacity = CAP;
    if (!buf) { capacity = 8192; buf = (char*)malloc(capacity); }
    pending = DiagBuffer(buf, buf ? capacity : 0);
    boot = (char*)heap_caps_malloc(BOOT_CAP, MALLOC_CAP_SPIRAM);
    mtx = xSemaphoreCreateMutex();
    if (esp_reset_reason() != ESP_RST_POWERON && storageTrace.magic == TRACE_MAGIC &&
        storageTrace.value == ~storageTrace.inverted) {
        log("sd prior RTC: stage=%u attempt=%u r1=0x%02x uptime=%lums "
            "(1 boot/2 probe/3 mount/4 ready/5 failed/6 unmount/7 sleep)",
            (unsigned)(storageTrace.value >> 16), (unsigned)((storageTrace.value >> 8) & 255),
            (unsigned)(storageTrace.value & 255), (unsigned long)storageTrace.uptime);
    }
    storageStage(1);
    Preferences prefs;
    if (prefs.begin("powerdiag", true)) {
        size_t n = prefs.getBytesLength("last");
        if (n > 0 && n <= SNAPSHOT_CAP) {
            previousLen = snprintf(previous, sizeof(previous),
                                   "[diag] prior failure snapshot (may be older than last boot):\n");
            previousLen += prefs.getBytes("last", previous + previousLen, n);
        }
        prefs.end();
    }
}

void log(const char* fmt, ...) {
    char line[384];
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
    crash_report::recordLine(line, n);
    memfault_service::recordLine(line, n);
    Serial.print(line);

    if (!buf || !mtx) return;
    xSemaphoreTake(mtx, portMAX_DELAY);
    pending.append(line, n);
    if (!bootFinished && boot && bootLen + n <= BOOT_CAP) {
        memcpy(boot + bootLen, line, n);
        bootLen += n;
    }
    xSemaphoreGive(mtx);
}

void finishBoot() {
    if (!mtx) return;
    xSemaphoreTake(mtx, portMAX_DELAY);
    bootFinished = true;
    xSemaphoreGive(mtx);
}

// One bounded flash write per boot, only when there is unpersisted evidence.
// Never write from log(), an ISR, or an SD driver callback. NVS is independent
// of the card, so a subsequent power cycle does not erase the mount failure.
void checkpoint(const char* reason) {
    if (!mtx || !buf) return;
    char* snapshot = (char*)malloc(SNAPSHOT_CAP);
    if (!snapshot) return;
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (checkpointSaved || pending.size == 0) {
        xSemaphoreGive(mtx); free(snapshot); return;
    }
    checkpointSaved = true;
    size_t prefix = snprintf(snapshot, SNAPSHOT_CAP, "[checkpoint] %s\n", reason);
    size_t count = pending.size;
    if (count > SNAPSHOT_CAP - prefix) count = SNAPSHOT_CAP - prefix;
    size_t start = pending.size - count;
    // Omit a partial first line if the snapshot had to take only the tail.
    if (start) {
        while (start < pending.size && pending.data[start - 1] != '\n') ++start;
        count = pending.size - start;
    }
    memcpy(snapshot + prefix, pending.data + start, count);
    xSemaphoreGive(mtx);
    power_mgmt::busyAcquire();
    Preferences prefs;
    bool ok = prefs.begin("powerdiag", false);
    if (ok) {
        ok = prefs.putBytes("last", snapshot, prefix + count) == prefix + count;
        prefs.end();
    }
    power_mgmt::busyRelease();
    free(snapshot);
    log("diag: flash checkpoint %s (%s)", ok ? "saved" : "FAILED", reason);
}

void dumpToSerial() {
    if (!mtx || !buf) { Serial.println("[diag] RAM log unavailable"); return; }
    // Snapshot under the mutex, print after releasing it: a slow/unplugged USB
    // reader must not block every task that logs while owning a bus mutex.
    char* copy = (char*)heap_caps_malloc(pending.capacity + BOOT_CAP + sizeof(previous) + 256,
                                       MALLOC_CAP_8BIT);
    if (!copy) { Serial.println("[diag] snapshot allocation failed"); return; }
    xSemaphoreTake(mtx, portMAX_DELAY);
    size_t n = snprintf(copy, 256, "[diag] pending=%u dropped=%lu; boot section may repeat recent lines\n",
                        (unsigned)pending.size, (unsigned long)pending.dropped);
    memcpy(copy + n, previous, previousLen); n += previousLen;
    if (boot) { memcpy(copy + n, boot, bootLen); n += bootLen; }
    memcpy(copy + n, pending.data, pending.size); n += pending.size;
    xSemaphoreGive(mtx);
    size_t sent = 0;
    while (sent < n) {
        size_t chunk = n - sent < 512 ? n - sent : 512;
        size_t wrote = Serial.write((const uint8_t*)copy + sent, chunk);
        sent += wrote;
        if (wrote != chunk) break;
    }
    Serial.println(sent == n ? "[diag] end" : "[diag] USB short write; retry diag");
    free(copy);
}

void flushToSD() {
    drainDriverLogs();
    if (!buf || !mtx || !ride_recorder::sdMounted()) return;
    // All flush callers serialize through SD -> diag, matching log callers.
    sdLock();
    if (!ride_recorder::sdMounted()) { sdUnlock(); return; }
    xSemaphoreTake(mtx, portMAX_DELAY);
    if ((retryWaiting && (int32_t)(millis() - retryAfterMs) < 0) ||
        (pending.size == 0 && previousLen == 0) ||
        (ride_recorder::isRecording() && pending.size < pending.capacity / 2 && millis()-lastFlushMs < 30000)) {
        xSemaphoreGive(mtx); sdUnlock(); return;
    }
    // If a long outage evicted boot lines, replay the protected boot copy once.
    // Its separate marker makes duplicate lines explicit. Append at most once;
    // short writes leave the remainder of the replay in the pending buffer.
    if (pending.dropped && !bootReplayDone && bootLen) {
        static const char marker[] = "[diag] retained boot after log overflow:\n";
        pending.append(marker, sizeof(marker) - 1);
        pending.append(boot, bootLen);
        bootReplayDone = true;
    }
    if (!SD.exists(LOG_DIR)) SD.mkdir(LOG_DIR);
    computeLogPath(activePath, sizeof(activePath));
    File f = SD.open(activePath, FILE_APPEND);
    bool failed = !f;
    if (f) {
        if (previousLen) {
            size_t wrote = f.write((const uint8_t*)previous, previousLen);
            if (wrote > previousLen) wrote = 0;
            failed = wrote != previousLen;
            memmove(previous, previous + wrote, previousLen - wrote);
            previousLen -= wrote;
        }
        if (!failed && pending.size) {
            size_t requested = pending.size;
            size_t wrote = f.write((const uint8_t*)pending.data, requested);
            if (wrote > requested) wrote = 0;
            pending.consume(wrote);
            failed = wrote != requested;
        }
        f.flush();
        f.close();
    }
    if (!failed) lastFlushMs = millis();
    retryWaiting = failed;
    retryAfterMs = millis() + 30000;
    xSemaphoreGive(mtx);
    sdUnlock();
    drainDriverLogs();
    if (failed) {
        log("diag: SD open/short write FAILED; unwritten bytes retained, retry in 30s");
        checkpoint("SD log write failed");
    }
}

const char* logPath() {
    computeLogPath(activePath, sizeof(activePath));
    return activePath;
}

}  // namespace diag
