#include "segments.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>

#include <atomic>

#include "diag.h"
#include "sd_bus.h"

#define SEG_DIR "/segments"
#define SEG_FILE SEG_DIR "/starred.bin"
#define SEG_TMP SEG_DIR "/starred.tmp"

namespace segments {
namespace {

// The parsed set waiting for the UI task. Guarded by `lock`: submit() runs in
// the BLE server task (or setup), takePending() in the UI task.
seg::SegmentDb pending;
bool havePending = false;
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
std::atomic<int> lastCount{-1};

// Parse outside the lock (it allocates), then swap in under it. The swap
// leaves the previous pending set in `db`, freed when it goes out of scope.
bool accept(const uint8_t* blob, size_t len, const char** err) {
    seg::SegmentDb db;
    if (!seg::parseBlob(blob, len, db, err)) return false;
    const int n = db.count;
    portENTER_CRITICAL(&lock);
    pending.swap(db);
    havePending = true;
    portEXIT_CRITICAL(&lock);
    lastCount = n;
    return true;
}

}  // namespace

void begin() {
    sdLock();
    if (!SD.exists(SEG_DIR)) SD.mkdir(SEG_DIR);
    File f = SD.open(SEG_FILE, FILE_READ);
    if (!f) {
        sdUnlock();
        return;
    }
    const size_t len = f.size();
    uint8_t* buf = len > 0 && len <= seg::BLOB_MAX
                       ? (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM)
                       : nullptr;
    const bool read = buf && f.read(buf, len) == (int)len;
    f.close();
    sdUnlock();
    const char* err = "unreadable";
    if (read && accept(buf, len, &err))
        diag::log("segments: %d starred loaded from SD", lastCount.load());
    else
        diag::log("segments: %s ignored (%s)", SEG_FILE, err);
    free(buf);
}

bool submit(const uint8_t* blob, size_t len, const char** err) {
    if (!accept(blob, len, err)) {
        diag::log("segments: upload rejected (%s)", *err);
        return false;
    }
    // Keep it for the next boot. A failed write still leaves the set live
    // for this one.
    sdLock();
    bool saved = false;
    if (SD.exists(SEG_DIR) || SD.mkdir(SEG_DIR)) {
        File f = SD.open(SEG_TMP, FILE_WRITE);
        if (f) {
            saved = f.write(blob, len) == len;
            f.close();
            if (saved) {
                SD.remove(SEG_FILE);
                saved = SD.rename(SEG_TMP, SEG_FILE);
            }
        }
    }
    sdUnlock();
    diag::log("segments: %d starred received (%u bytes)%s", lastCount.load(),
              (unsigned)len, saved ? "" : ", not saved to SD");
    return true;
}

int count() { return lastCount; }

bool takePending(seg::SegmentDb& live) {
    if (!havePending) return false;
    seg::SegmentDb old;
    portENTER_CRITICAL(&lock);
    live.swap(pending);   // live = the new set, pending = the old one
    pending.swap(old);    // pending = empty; the old set is freed below,
    havePending = false;  // outside the lock
    portEXIT_CRITICAL(&lock);
    return true;
}

}  // namespace segments
