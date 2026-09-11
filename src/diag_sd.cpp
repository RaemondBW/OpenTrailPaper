#include "diag.h"
#include <Arduino.h>
#include <SD.h>
#include "ride_recorder.h"
#include "sd_bus.h"
#include "usb_storage.h"

// Read-only serial retrieval: avoid USB mass-storage ownership and preserve
// byte boundaries even when other tasks log concurrently. Each chunk is one
// hex-encoded serial line. Never wait on USB while holding the shared SPI bus.
void diag::dumpSDToSerial(size_t maxBytes) {
    if (maxBytes < 256 || maxBytes > 262144) {
        Serial.println("[sdlog] use diag sd [256..262144 bytes]"); return;
    }
    if (!ride_recorder::sdMounted() || ride_recorder::isRecording() || usb_storage::hostActive()) {
        Serial.println("[sdlog] unavailable: card missing, recording, or USB host owns card; use diag for retained evidence");
        return;
    }
    char path[48];
    sdLock();
    snprintf(path, sizeof(path), "%s", diag::logPath());
    File f = SD.open(path, FILE_READ);
    bool opened = (bool)f;
    size_t end = opened ? f.size() : 0;
    if (f) f.close();
    sdUnlock();
    if (!opened) { Serial.println("[sdlog] open failed; use diag for retained evidence"); return; }
    size_t offset = end > maxBytes ? end - maxBytes : 0;
    Serial.printf("[sdlog] begin path=%s start=%u end=%u encoding=hex\n", path, (unsigned)offset, (unsigned)end);
    const uint32_t started = millis();
    while (offset < end && millis() - started < 30000 && Serial) {
        uint8_t bytes[128];
        size_t wanted = end - offset < sizeof(bytes) ? end - offset : sizeof(bytes);
        size_t got = 0;
        sdLock();
        // Re-open for each chunk so remounts never invalidate a retained File.
        if (ride_recorder::sdMounted() && !ride_recorder::isRecording() && !usb_storage::hostActive()) {
            f = SD.open(path, FILE_READ);
            if (f && f.seek(offset)) got = f.read(bytes, wanted);
            if (f) f.close();
        }
        sdUnlock();
        if (!got || got > wanted) break;
        char line[300];
        int n = snprintf(line, sizeof(line), "[sdlog] data %08x ", (unsigned)offset);
        const char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < got; ++i) {
            line[n++] = hex[bytes[i] >> 4]; line[n++] = hex[bytes[i] & 15];
        }
        line[n++] = '\n';
        if (Serial.write((const uint8_t*)line, n) != (size_t)n) break;
        offset += got;
        delay(1); // let idle/USB/peripheral tasks run during a large log dump
    }
    Serial.printf("[sdlog] end offset=%u expected=%u complete=%d\n", (unsigned)offset, (unsigned)end, offset == end);
    diag::drainDriverLogs();
}
