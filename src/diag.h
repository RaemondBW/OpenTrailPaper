#pragma once

#include <stddef.h>

// Lightweight diagnostic log. Events are timestamped and appended to a PSRAM
// buffer (safe to call from any task); a single owner (the BLE server task)
// periodically flushes the buffer to /diag.log on the SD card when the recorder
// isn't using the bus. The phone can download /diag.log to inspect issues.
namespace diag {

void begin();

// printf-style; also echoed to Serial. Keep messages short.
void log(const char* fmt, ...);

// Append buffered lines to /diag.log. No-op while recording or if SD is busy.
void flushToSD();

const char* logPath();   // "/diag.log"

}
