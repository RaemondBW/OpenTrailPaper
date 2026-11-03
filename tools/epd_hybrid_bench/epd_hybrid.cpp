#include "epd_hybrid.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <new>

using namespace hybrid;


extern "C" void epd_painter_convert_packed_fb_to_ink_or(const uint8_t* packed_fb, uint8_t* output,
                                                        uint32_t length, const uint8_t* waveform,
                                                        uint32_t chunk_flags);

namespace {

uint8_t* psramPlane() {
    auto* p = (uint8_t*)heap_caps_aligned_alloc(kChunkBytes, kPlaneBytes, MALLOC_CAP_SPIRAM);
    if (p) memset(p, 0, kPlaneBytes);
    return p;
}

// Dose is the time between a row's latches, so the deadline has to be real:
// sleep while there is plenty of time, then spin. Same reasoning as the driver's
// epd_wait_until_us(), which is file-local there.
void waitUntil(int64_t deadline) {
    while (deadline - esp_timer_get_time() > 2000) vTaskDelay(1);
    while (esp_timer_get_time() < deadline) {}
}

// Roughly what fits in the gap between passes at 8 us a chunk.
constexpr int kFloatAboveChunks = 400;

constexpr int kFrameGapMs = 5;

}  // namespace

bool EpdHybrid::begin() {
    void* mem = heap_caps_malloc(sizeof(Core), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!mem) return false;
    core_ = new (mem) Core;

    uint8_t* planes[kCohorts + kGenerations + 2];
    for (uint8_t*& p : planes) {
        p = psramPlane();
        if (!p) return false;
    }
    core_->begin(planes);
    mail_ = psramPlane();
    shadow_ = psramPlane();
    mailLock_ = xSemaphoreCreateMutex();
    if (!mail_ || !shadow_ || !mailLock_) return false;

    // Where and how fast the driver runs its own paint task: core 0, above
    // everything else, because this loop is doing the same job.
    return xTaskCreatePinnedToCore(taskEntry, "epd_hybrid", 8192, this, 10, nullptr, 0) == pdPASS;
}

void EpdHybrid::engage() {
    while (!painter_.paintIdle()) vTaskDelay(1);
    core_->reset(millis());
    memset(shadow_, 0, kPlaneBytes);
    memset(mailDirty_, 0, sizeof mailDirty_);
    mailFull_ = false;
    passes_ = overruns_ = floats_ = posted_ = started_ = 0;
    maxPassUs_ = maxSubmitUs_ = startDelaySumUs_ = startDelayMaxUs_ = 0;
    engaged_ = true;
    while (parked_) vTaskDelay(1);
}

void EpdHybrid::disengage() {
    while (!quiet()) vTaskDelay(5);
    engaged_ = false;
    while (!parked_) vTaskDelay(1);
}

void EpdHybrid::post(const uint8_t* frame) {
    xSemaphoreTake(mailLock_, portMAX_DELAY);
    bool any = false;
    for (int row = 0; row < kHeight; ++row) {
        const size_t rowAt = (size_t)row * kRowBytes;
        uint32_t m = 0;
        for (int chunk = 0; chunk < kChunks; ++chunk) {
            const size_t at = rowAt + chunk * kChunkBytes;
            if (memcmp(frame + at, shadow_ + at, kChunkBytes) == 0) continue;
            memcpy(shadow_ + at, frame + at, kChunkBytes);
            memcpy(mail_ + at, frame + at, kChunkBytes);
            m |= chunkBit(chunk);
        }
        // OR, not assign: an earlier frame may not have been picked up yet. mail_
        // always equals shadow_, so its chunks are the newest contents regardless.
        mailDirty_[row] |= m;
        any |= mailDirty_[row] != 0;
    }
    posted_++;
    if (any) {
        if (!mailFull_) mailSince_ = esp_timer_get_time();
        mailFull_ = true;
    }
    xSemaphoreGive(mailLock_);
}

void EpdHybrid::takeMail() {
    if (!mailFull_ || (batch && core_->driving())) return;
    // Never wait here. post() can hold this for milliseconds and the pass clock
    // is running: the frame just starts one pass later.
    if (xSemaphoreTake(mailLock_, 0) != pdTRUE) return;
    // While discovery runs, every row keeps driving what it last latched, and dose
    // is the time until the next latch. A big frame takes tens of ms, so without
    // this every pixel mid-drive would get several passes' dose in one. Float the
    // panel first: the frame costs a scan, and the dose stays exact.
    int chunks = 0;
    for (int row = 0; row < kHeight; ++row) chunks += __builtin_popcount(mailDirty_[row]);
    if (wireLive_ && chunks > kFloatAboveChunks) {
        neutralFrame();
        wireLive_ = false;
        floats_++;
    }
    const int64_t t0 = esp_timer_get_time();
    if (core_->submit(mail_, mailDirty_, millis())) {
        memset(mailDirty_, 0, sizeof mailDirty_);
        mailFull_ = false;
        const int64_t t1 = esp_timer_get_time();
        if (t1 - t0 > maxSubmitUs_) maxSubmitUs_ = t1 - t0;
        const int64_t delay = t1 - mailSince_;
        startDelaySumUs_ += delay;
        if (delay > startDelayMaxUs_) startDelayMaxUs_ = delay;
        started_++;
    }
    xSemaphoreGive(mailLock_);
}

void EpdHybrid::scanPass() {
    struct Sweep {
        const uint8_t* plane;
        const uint32_t* mask;
        uint8_t table[3];   // the kernel's order: slot 3, 2, 1
    } sweeps[kCohorts];
    int n = 0;
    for (int i = 0; i < kCohorts; ++i) {
        const Cohort& c = core_->cohort(i);
        if (c.kind == Kind::Free) continue;
        sweeps[n++] = {c.plane, c.mask,
                       {0, (uint8_t)(Core::code(c, 2) * 0x55), (uint8_t)(Core::code(c, 1) * 0x55)}};
    }

    // Every row goes out every pass, floated or not: skipping one would change
    // when its neighbours latch, and with it their dose.
    for (int row = 0; row < kHeight; ++row) {
        memset(painter_.dma_buffer, 0, kRowBytes);
        for (int s = 0; s < n; ++s) {
            const uint32_t m = sweeps[s].mask[row];
            if (m) {
                epd_painter_convert_packed_fb_to_ink_or(sweeps[s].plane + (size_t)row * kRowBytes,
                                                        painter_.dma_buffer, kRowBytes,
                                                        sweeps[s].table, m);
            }
        }
        painter_.sendRow(row == 0, false, false);
    }
}

// All zeros, then the final latch: stops the pixels of the last row holding
// their drive voltage. The driver ends every drive cycle the same way.
void EpdHybrid::neutralFrame() {
    memset(painter_.dma_buffer1, 0, kRowBytes);
    memset(painter_.dma_buffer2, 0, kRowBytes);
    for (int row = 0; row < kHeight; ++row) painter_.sendRow(row == 0, row == kHeight - 1);
}

void EpdHybrid::taskEntry(void* self) { static_cast<EpdHybrid*>(self)->task(); }

void EpdHybrid::task() {
    int64_t lastYield = esp_timer_get_time();
    for (;;) {
        if (!engaged_) {
            parked_ = true;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        parked_ = false;

        // A frame has just finished. Everything else on this core gets its turn here,
        // with the panel floated so no pixel's dose depends on how long that takes.
        // Without this a frame that is always waiting would hold the core solid.
        if (batch && wireLive_ && !core_->driving()) {
            neutralFrame();
            wireLive_ = false;
            vTaskDelay(pdMS_TO_TICKS(kFrameGapMs));
        }

        const int64_t t0 = esp_timer_get_time();
        takeMail();
        core_->age(millis());
        if (!core_->driving()) {
            if (wireLive_) {
                neutralFrame();
                wireLive_ = false;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        {
            EPD_Painter::PanelPowerGuard guard(painter_);
            const int64_t scanStart = esp_timer_get_time();
            scanPass();
            scanSumUs_ += esp_timer_get_time() - scanStart;
            scans_++;
        }
        wireLive_ = true;
        passes_++;
        core_->advance();

        const int64_t elapsed = esp_timer_get_time() - t0;
        if (elapsed > maxPassUs_) maxPassUs_ = elapsed;
        if (elapsed > passUs) overruns_++;
        waitUntil(t0 + passUs);
        // Pinned at this priority nothing else on the core runs unless we sleep,
        // and passes that have no time to spare would starve the idle task until
        // the watchdog resets the chip. One late pass a second is a much smaller
        // error than that. Counted in time, because a slow pass is exactly when
        // the task has stopped sleeping on its own.
        const int64_t now = esp_timer_get_time();
        if (now - lastYield > 500000) {
            vTaskDelay(2);
            lastYield = esp_timer_get_time();
        }
    }
}

EpdHybrid::Counters EpdHybrid::counters() const {
    const Stats& s = core_->stats();
    return {posted_,    s.frames,  s.refused,  passes_,      overruns_,        floats_,    s.cancelled,
            s.deferred, s.settles, maxPassUs_, maxSubmitUs_, startDelayMaxUs_,
            scans_ ? scanSumUs_ / scans_ : 0, s.peakCohorts};
}

void EpdHybrid::resetPeaks() {
    maxPassUs_ = maxSubmitUs_ = startDelayMaxUs_ = 0;
    scanSumUs_ = 0;
    scans_ = 0;
    core_->resetPeak();
}

void EpdHybrid::printStats() const {
    const Stats& s = core_->stats();
    Serial.printf("[hybrid] frames %u posted %u accepted %u refused, peak %d cohorts\n",
                  (unsigned)posted_, (unsigned)s.frames, (unsigned)s.refused,
                  (int)s.peakCohorts);
    Serial.printf("[hybrid] %u passes at %lld us: %u overran, slowest %lld us, slowest submit %lld us\n",
                  (unsigned)passes_, (long long)passUs, (unsigned)overruns_,
                  (long long)maxPassUs_, (long long)maxSubmitUs_);
    Serial.printf("[hybrid] post to first pass: mean %lld us, worst %lld us\n",
                  (long long)(started_ ? startDelaySumUs_ / started_ : 0),
                  (long long)startDelayMaxUs_);
    Serial.printf("[hybrid] %u reversed words, %u deferred, %u settles\n", (unsigned)s.cancelled,
                  (unsigned)s.deferred, (unsigned)s.settles);
}
