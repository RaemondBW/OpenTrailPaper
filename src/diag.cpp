#include "diag.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <time.h>

#include "ride_recorder.h"
#include "sd_bus.h"

namespace {

char* buf = nullptr;
size_t len = 0;
constexpr size_t CAP = 48 * 1024;         // in-RAM staging before an SD flush
SemaphoreHandle_t mtx = nullptr;
constexpr char LOG_DIR[] = "/logs";
char activePath[48] = "/logs/pending.log";

// One log file per day (UTC): /logs/YYYYMMDD.log — small and easy to grab.
// Before the clock is set (no GPS fix yet) lines go to /logs/pending.log.
void computeLogPath(char* out, size_t n) {
    time_t now = time(nullptr);
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
    time_t now = time(nullptr);
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
    if (!ride_recorder::sdMounted() || ride_recorder::isRecording()) return;
    xSemaphoreTake(mtx, portMAX_DELAY);
    if (len == 0) { xSemaphoreGive(mtx); return; }
    sdLock();
    if (!SD.exists(LOG_DIR)) SD.mkdir(LOG_DIR);
    computeLogPath(activePath, sizeof(activePath));   // today's file (UTC)
    File f = SD.open(activePath, FILE_APPEND);
    if (f) {
        f.write((const uint8_t*)buf, len);
        f.close();
        len = 0;
    }
    sdUnlock();
    xSemaphoreGive(mtx);
}

const char* logPath() {
    computeLogPath(activePath, sizeof(activePath));
    return activePath;
}

}  // namespace diag
