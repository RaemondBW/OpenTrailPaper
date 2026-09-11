#pragma once

#include <stddef.h>
#include <stdint.h>

// Timestamped Serial + buffered SD diagnostics. SD flushes serialize across
// callers; an 8 KiB boot copy and bounded NVS failure snapshot keep evidence
// available when the card is absent. The phone can download daily /logs files.
namespace diag {

// RTC breadcrumbs survive deep sleep/software reset, not removal of power.
// Stages: 1 boot, 2 probe, 3 SD.begin, 4 mounted, 5 failed, 6 SD.end, 7 asleep.
void storageStage(uint8_t stage, uint8_t attempt = 0, uint8_t r1 = 0xff);
void begin();
void drainDriverLogs(); // called outside logger/SD locks
void finishBoot(); // freeze protected boot copy for later serial retrieval
void dumpToSerial(); // works without a card; never consumes buffered data
void dumpSDToSerial(size_t maxBytes = 131072); // bounded read-only daily-log tail
void checkpoint(const char* reason); // bounded NVS fallback, at most once/boot

// printf-style; also echoed to Serial. Keep messages short.
void log(const char* fmt, ...);

// Append to /logs/YYYYMMDD.log. Retain short writes; defer small ride flushes.
void flushToSD();

const char* logPath();   // current /logs/YYYYMMDD.log (or pending.log before clock sync)

}
