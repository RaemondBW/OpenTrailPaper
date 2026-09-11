#include <Arduino.h>
#include <stdarg.h>
#include "diag.h"
#include "diag_buffer.h"

namespace {
char storage[4096];
DiagBuffer queued(storage, sizeof(storage));
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
}

// Arduino log_w/log_e use log_printf(), NOT esp_log_set_vprintf(). Linker
// wrapping keeps this project-local; no edits to the installed SD framework.
// Never take the diag mutex here: SD can log while flushToSD() already owns it.
extern "C" int log_printfv(const char* format, va_list args);
extern "C" int __wrap_log_printf(const char* format, ...) {
    char line[300];
    va_list args, copy;
    va_start(args, format);
    va_copy(copy, args);
    int length = vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);
    if (strstr(line, "sd_diskio.cpp:") || strstr(line, "SD.cpp:") ||
        strstr(line, "vfs_api.cpp:")) {
        size_t n = strnlen(line, sizeof(line) - 1);
        if (n && line[n - 1] != '\n') line[n - 1] = '\n';
        portENTER_CRITICAL(&mux);
        queued.append(line, n);
        portEXIT_CRITICAL(&mux);
        // drainDriverLogs echoes these to CDC and the persistent logger. Avoid
        // the default UART0 route, which shares the GPS pins on this board.
    } else {
        length = log_printfv(format, args);
    }
    va_end(args);
    return length;
}

void diag::drainDriverLogs() {
    // One bounded line at a time; log outside the spinlock. Multiple drainers
    // may interleave output, but can never duplicate or consume the same line.
    for (unsigned i = 0; i < 32; ++i) {
        char line[300];
        portENTER_CRITICAL(&mux);
        size_t n = 0;
        while (n < queued.size && n < sizeof(line) - 1) {
            if (queued.data[n++] == '\n') break;
        }
        memcpy(line, queued.data, n);
        queued.consume(n);
        uint32_t dropped = queued.dropped;
        queued.dropped = 0;
        portEXIT_CRITICAL(&mux);
        if (dropped) diag::log("sd driver: %lu log bytes dropped", (unsigned long)dropped);
        if (!n) break;
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) --n;
        line[n] = 0;
        diag::log("sd driver: %s", line);
    }
}
