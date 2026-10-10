#include "diag.h"
#include <Arduino.h>
#include <SD.h>
#include <atomic>
#include <ctype.h>
#include <esp_heap_caps.h>
#include "ride_recorder.h"
#include "sd_bus.h"
#include "usb_storage.h"

// Read-only serial retrieval: avoid USB mass-storage ownership and preserve
// byte boundaries even when other tasks log concurrently. Each chunk is one
// hex-encoded serial line. Never wait on USB while holding the shared SPI bus.
//
// Runs on its own short-lived task, not the UI task that parsed the command.
// A 256 KiB dump made the USB CDC port vanish twice at ~110 KB (2026-10-10).
// The CDC TX buffer is 64 bytes, and USBCDC::write() busy-spins
// (flush/continue, no yield) holding its TX lock until the host drains it. A
// 280-byte line therefore spun the UI task through every line for the whole
// dump, and any other task that logged meanwhile waited 250 ms on the lock per
// line. The UI task's 8 KB stack was also carrying FatFS (LFN buffers on the
// stack in this build) under the console parser. Now:
//  * lines fit the 64-byte buffer (20 data bytes each) and are written only
//    when that much space is free, so a write never spins and a line is
//    still one write (other tasks' output can't land inside it);
//  * the task yields between lines and backs off while the host is slow;
//  * the card is read 4 KiB at a time into a heap buffer;
//  * a stall (host stopped reading) ends the dump with the offset to resume
//    from, instead of a fixed 30 s cap that cut long dumps short.
namespace {

constexpr size_t kLineBytes = 20;     // "[sdlog] data %08x " + 40 hex + '\n' = 63
constexpr size_t kBlock = 4096;
constexpr uint32_t kStallMs = 5000;

struct Job {
    char path[48];
    size_t start;
    size_t end;
};

std::atomic<bool> busy{false};

bool cardFree() {
    return ride_recorder::sdMounted() && !ride_recorder::isRecording() &&
           !usb_storage::hostActive();
}

// Write a whole line once the CDC buffer has room for it. false: the host
// went away or stopped reading for kStallMs.
bool writeLine(const char* line, size_t n) {
    uint32_t waitStart = millis();
    while ((size_t)Serial.availableForWrite() < n) {
        if (!Serial || millis() - waitStart > kStallMs) return false;
        vTaskDelay(1);
    }
    return Serial.write((const uint8_t*)line, n) == n;
}

void dumpTask(void* arg) {
    Job* job = (Job*)arg;
    uint8_t* block = (uint8_t*)heap_caps_malloc(kBlock, MALLOC_CAP_8BIT);
    size_t offset = job->start;
    bool ok = block != nullptr;
    if (!block) Serial.println("[sdlog] buffer allocation failed");
    else Serial.printf("[sdlog] begin path=%s start=%u end=%u encoding=hex\n",
                       job->path, (unsigned)job->start, (unsigned)job->end);
    while (ok && offset < job->end) {
        size_t wanted = job->end - offset < kBlock ? job->end - offset : kBlock;
        size_t got = 0;
        sdLock();
        // Re-open for each block so remounts never invalidate a retained File.
        if (cardFree()) {
            File f = SD.open(job->path, FILE_READ);
            if (f && f.seek(offset)) got = f.read(block, wanted);
            if (f) f.close();
        }
        sdUnlock();
        if (!got || got > wanted) { ok = false; break; }
        for (size_t i = 0; i < got && ok; i += kLineBytes) {
            size_t len = got - i < kLineBytes ? got - i : kLineBytes;
            char line[72];
            int n = snprintf(line, sizeof(line), "[sdlog] data %08x ",
                             (unsigned)(offset + i));
            static const char hex[] = "0123456789abcdef";
            for (size_t k = 0; k < len; ++k) {
                line[n++] = hex[block[i + k] >> 4];
                line[n++] = hex[block[i + k] & 15];
            }
            line[n++] = '\n';
            if (!writeLine(line, n)) { ok = false; break; }
            offset += len;
            vTaskDelay(1);   // idle, USB and the other tasks run between lines
        }
    }
    if (block) {
        Serial.printf("[sdlog] end offset=%u expected=%u complete=%d\n",
                      (unsigned)offset, (unsigned)job->end, offset == job->end);
        if (offset != job->end) {
            const char* base = strrchr(job->path, '/');
            Serial.printf("[sdlog] stopped early; resume: diag sd %s %u %u\n",
                          base ? base + 1 : job->path,
                          (unsigned)(job->end - offset), (unsigned)offset);
        }
    }
    heap_caps_free(block);
    delete job;
    diag::drainDriverLogs();
    busy = false;
    vTaskDelete(nullptr);
}

}  // namespace

bool diag::validLogName(const char* name) {
    if (!name || !*name || strlen(name) > 32 || name[0] == '.') return false;
    for (const char* p = name; *p; ++p)
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-')
            return false;
    return true;
}

void diag::listSDLogs() {
    if (!cardFree()) {
        Serial.println("[sdlog] unavailable: card missing, recording, or USB host owns card");
        return;
    }
    sdLock();
    File dir = SD.open("/logs");
    int n = 0;
    if (dir) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            if (!f.isDirectory()) {
                const char* base = strrchr(f.name(), '/');
                Serial.printf("[sdlog] file %s %u\n", base ? base + 1 : f.name(),
                              (unsigned)f.size());
                ++n;
            }
            f.close();
            if ((n & 15) == 0) vTaskDelay(1);
        }
        dir.close();
    }
    sdUnlock();
    Serial.printf("[sdlog] %d file(s); today's is %s\n", n, diag::logPath());
}

// name: a file in /logs, or nullptr for today's. offset < 0: the last
// maxBytes; otherwise maxBytes starting at offset.
void diag::dumpSDToSerial(const char* name, long offset, size_t maxBytes) {
    if (maxBytes < 256 || maxBytes > 262144) {
        Serial.println("[sdlog] use diag sd [file] [256..262144 bytes] [offset]"); return;
    }
    if (name && !validLogName(name)) {
        Serial.println("[sdlog] file must be a name in /logs (diag sd ls)"); return;
    }
    if (!cardFree()) {
        Serial.println("[sdlog] unavailable: card missing, recording, or USB host owns card; use diag for retained evidence");
        return;
    }
    if (busy.exchange(true)) { Serial.println("[sdlog] a dump is already running"); return; }
    Job* job = new Job;
    if (name) snprintf(job->path, sizeof(job->path), "/logs/%s", name);
    else snprintf(job->path, sizeof(job->path), "%s", diag::logPath());
    sdLock();
    File f = SD.open(job->path, FILE_READ);
    bool opened = (bool)f;
    size_t size = opened ? f.size() : 0;
    if (f) f.close();
    sdUnlock();
    if (!opened) {
        Serial.printf("[sdlog] open %s failed; diag sd ls lists the files\n", job->path);
        delete job; busy = false; return;
    }
    if (offset < 0) {
        job->start = size > maxBytes ? size - maxBytes : 0;
        job->end = size;
    } else {
        job->start = (size_t)offset < size ? (size_t)offset : size;
        job->end = size - job->start > maxBytes ? job->start + maxBytes : size;
    }
    // 8 KB: FatFS keeps its long-file-name buffer on the caller's stack here.
    // Below the UI task's priority, on its core, away from the BLE host.
    if (xTaskCreatePinnedToCore(dumpTask, "sdlog", 8192, job, 1, nullptr, 1) != pdPASS) {
        Serial.println("[sdlog] could not start the dump task (low memory)");
        delete job; busy = false;
    }
}
