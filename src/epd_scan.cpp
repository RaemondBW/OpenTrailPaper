#ifdef USE_EPD_PAINTER

#include "epd_scan.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>

using namespace scan;

extern "C" void epd_painter_convert_packed_fb_to_ink_or(const uint8_t* packed_fb, uint8_t* output,
                                                        uint32_t length, const uint8_t* waveform,
                                                        uint32_t chunk_flags);

namespace {

// Judged on the glass. These move together: draw and erase are matched so the charge
// balances, and an erase that goes further into white needs a draw that comes further
// back, or black goes grey. 7 and 2 left ghosts on real screens, 7 and 4 lost black.
constexpr uint8_t kDrivePasses = 9;
constexpr uint8_t kErasePrime = 3;
constexpr int64_t kPassUs = 10000;
constexpr int kFrameGapMs = 5;

// Dose is the time between a row's latches, so the deadline has to be real: sleep
// while there is plenty of time, then spin.
void waitUntil(int64_t deadline) {
    while (deadline - esp_timer_get_time() > 2000) vTaskDelay(1);
    while (esp_timer_get_time() < deadline) {}
}

}  // namespace

void EpdScan::buildPlan() {
    const EPD_Painter::Config& cfg = painter_._config;
    plan_.build({cfg.waveforms.normal_darker, cfg.waveforms.normal_lighter, cfg.trains.dir_normal},
                drivePasses_, prime_);
}

bool EpdScan::tune(int drivePasses, int prime) {
    if (drivePasses < 1 || prime < 0 || drivePasses + 2 * prime > kMaxTrain) return false;
    waitIdle(2000);
    xSemaphoreTake(painter_._paint_active_sem, portMAX_DELAY);
    drivePasses_ = (uint8_t)drivePasses;
    prime_ = (uint8_t)prime;
    buildPlan();
    xSemaphoreGive(painter_._paint_active_sem);
    return true;
}

bool EpdScan::begin() {
    if (!painter_._config.trains.dir_normal) return false;
    drivePasses_ = kDrivePasses;
    prime_ = kErasePrime;
    buildPlan();
    if (!plan_.complete()) return false;

    for (int p = 0; p < kPlanes; ++p) {
        planes_[p] = (uint8_t*)heap_caps_aligned_alloc(kChunkBytes, kPlaneBytes, MALLOC_CAP_SPIRAM);
        masks_[p] = (uint32_t*)heap_caps_calloc(kHeight, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
        if (!planes_[p] || !masks_[p]) return false;
        memset(planes_[p], 0, kPlaneBytes);
    }
    mail_ = (uint8_t*)heap_caps_malloc(kPlaneBytes, MALLOC_CAP_SPIRAM);
    eraseTarget_ = (uint8_t*)heap_caps_malloc(kPlaneBytes, MALLOC_CAP_SPIRAM);
    mailLock_ = xSemaphoreCreateMutex();
    if (!mail_ || !eraseTarget_ || !mailLock_) return false;

    // Where and how high the driver runs its own paint task, because this is doing
    // the same job. Pass timing is what the look depends on.
    return xTaskCreatePinnedToCore(taskEntry, "epd_scan", 4096, this, 10, &task_, 0) == pdPASS;
}

// Call with mailLock_ held.
void EpdScan::pack(const uint8_t* fb4, const uint8_t* levels) {
    if (packFor_ != levels) {
        for (int b = 0; b < 256; ++b) pack_[b] = (uint8_t)((levels[b & 0x0F] << 2) | levels[b >> 4]);
        packFor_ = levels;
    }
    for (size_t i = 0; i < kPlaneBytes; ++i)
        mail_[i] = (uint8_t)((pack_[fb4[i * 2]] << 4) | pack_[fb4[i * 2 + 1]]);
}

void EpdScan::post(const uint8_t* fb4, const uint8_t* levels) {
    xSemaphoreTake(mailLock_, portMAX_DELAY);
    pack(fb4, levels);
    mailFull_ = true;
    xSemaphoreGive(mailLock_);
    xTaskNotifyGive(task_);
}

void EpdScan::eraseBits(const uint8_t* bits) {
    xSemaphoreTake(mailLock_, portMAX_DELAY);
    eraseBits_ = bits;
    eraseWanted_ = true;
    xSemaphoreGive(mailLock_);
    xTaskNotifyGive(task_);
    const uint32_t start = millis();
    while (eraseWanted_ && millis() - start < 2000) vTaskDelay(pdMS_TO_TICKS(2));
}

void EpdScan::eraseDirty(const uint8_t* fb4, const uint8_t* levels, int tolerance) {
    xSemaphoreTake(mailLock_, portMAX_DELAY);
    pack(fb4, levels);
    eraseBits_ = nullptr;
    eraseTolerance_ = tolerance;
    eraseWanted_ = true;
    xSemaphoreGive(mailLock_);
    xTaskNotifyGive(task_);
    const uint32_t start = millis();
    while (eraseWanted_ && millis() - start < 2000) vTaskDelay(pdMS_TO_TICKS(2));
}

bool EpdScan::waitIdle(uint32_t timeoutMs) {
    const uint32_t start = millis();
    while (mailFull_ || eraseWanted_ || driving_) {
        if (millis() - start > timeoutMs) return false;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return true;
}

// Every row goes out every pass, floated or not: skipping one would change when its
// neighbours latch, and with it their dose.
void EpdScan::drive(int passes) {
    EPD_Painter::PanelPowerGuard guard(painter_);
    for (int pass = 0; pass < passes; ++pass) {
        const int64_t t0 = esp_timer_get_time();
        uint8_t tables[kPlanes][3];
        for (int p = 0; p < kPlanes; ++p) {
            for (int slot = 1; slot <= 3; ++slot)
                tables[p][3 - slot] = (uint8_t)(plan_.code(p, slot, pass) * 0x55);
        }
        for (int row = 0; row < kHeight; ++row) {
            memset(painter_.dma_buffer, 0, kRowBytes);
            for (int p = 0; p < kPlanes; ++p) {
                const uint32_t m = masks_[p][row];
                if (m) {
                    epd_painter_convert_packed_fb_to_ink_or(planes_[p] + (size_t)row * kRowBytes,
                                                            painter_.dma_buffer, kRowBytes,
                                                            tables[p], m);
                }
            }
            painter_.sendRow(row == 0, false, false);
        }
        const int64_t elapsed = esp_timer_get_time() - t0;
        if (elapsed > slowestPassUs_) slowestPassUs_ = elapsed;
        if (elapsed > kPassUs) overruns_++;
        waitUntil(t0 + kPassUs);
    }
    neutralFrame();
}

// All zeros, then the final latch, or the last row keeps its drive voltage. The driver
// ends every drive the same way.
void EpdScan::neutralFrame() {
    memset(painter_.dma_buffer1, 0, kRowBytes);
    memset(painter_.dma_buffer2, 0, kRowBytes);
    for (int row = 0; row < kHeight; ++row) painter_.sendRow(row == 0, row == kHeight - 1);
}

void EpdScan::taskEntry(void* self) { static_cast<EpdScan*>(self)->task(); }

// These run with the driver's paint lock held.
int EpdScan::discover(const uint8_t* target) {
    const int64_t t0 = esp_timer_get_time();
    const int passes = plan_.discover(target, painter_.packed_screenbuffer, planes_, masks_);
    const int64_t discovered = esp_timer_get_time() - t0;
    if (discovered > slowestDiscoverUs_) slowestDiscoverUs_ = discovered;
    return passes;
}

void EpdScan::finish(int passes) {
    if (!passes) return;
    drive(passes);
    frames_++;
    for (int p = 0; p < kPlanes; ++p) {
        if (!plan_.planeInUse(p)) continue;
        memset(planes_[p], 0, kPlaneBytes);
        memset(masks_[p], 0, kHeight * sizeof(uint32_t));
    }
}

// What is on the glass, with the rectangles that differ from the new frame made white.
int EpdScan::discoverErase() {
    erases_++;
    memcpy(eraseTarget_, painter_.packed_screenbuffer, kPlaneBytes);
    if (eraseBits_) {
        // A byte of bits covers eight framebuffer bytes, which pack into four: the first
        // framebuffer byte of each pair is the high nibble.
        static uint32_t keep[256];
        if (!keep[0]) {
            for (int b = 0; b < 256; ++b) {
                uint8_t bytes[4] = {0xFF, 0xFF, 0xFF, 0xFF};
                for (int j = 0; j < 8; ++j) {
                    if (b & (1 << j)) bytes[j / 2] &= (j & 1) ? 0xF0 : 0x0F;
                }
                memcpy(&keep[b], bytes, 4);
            }
        }
        uint32_t* word = (uint32_t*)eraseTarget_;
        for (size_t i = 0; i < kPlaneBytes / 4; ++i) {
            if (eraseBits_[i]) word[i] &= keep[eraseBits_[i]];
        }
        return discover(eraseTarget_);
    }

    xSemaphoreTake(painter_._paint_buffer_sem, portMAX_DELAY);
    memcpy(painter_.packed_paintbuffer, mail_, kPlaneBytes);
    xSemaphoreGive(painter_._paint_buffer_sem);

    EPD_Painter::Rect rects[32];
    const int n = painter_.computeDirtyRects(rects, 32, eraseTolerance_);
    for (int r = 0; r < n; ++r) {
        const int bx0 = rects[r].x / 4;
        int bx1 = (rects[r].x + rects[r].w + 3) / 4;
        if (bx1 > kRowBytes) bx1 = kRowBytes;
        for (int row = rects[r].y; row < rects[r].y + rects[r].h && row < kHeight; ++row)
            memset(eraseTarget_ + (size_t)row * kRowBytes + bx0, 0, bx1 - bx0);
    }
    return discover(eraseTarget_);
}

void EpdScan::task() {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (mailFull_ || eraseWanted_) {
            driving_ = true;
            // Holds the driver off the wire and its screenbuffer. A clear() waits here
            // until this frame is done, and this waits out one already running.
            xSemaphoreTake(painter_._paint_active_sem, portMAX_DELAY);

            // Only the compare needs the mail: post() must not wait out a whole drive.
            xSemaphoreTake(mailLock_, portMAX_DELAY);
            const bool erasing = eraseWanted_;
            mailFull_ = false;
            const int passes = erasing ? discoverErase() : discover(mail_);
            xSemaphoreGive(mailLock_);

            finish(passes);
            if (erasing) eraseWanted_ = false;
            xSemaphoreGive(painter_._paint_active_sem);
            driving_ = false;

            // Everything else on this core gets its turn between frames, with the panel
            // floated so no pixel's dose depends on how long that takes. Without this a
            // frame that is always waiting would hold the core solid.
            vTaskDelay(pdMS_TO_TICKS(kFrameGapMs));
        }
    }
}

void EpdScan::report() const {
    Serial.printf("[panelscan] draw %u passes, erase prime %u; %u frames, %u scrub erases, "
                  "%u passes overran, slowest pass %lld us, slowest discovery %lld us\n",
                  drivePasses_, prime_, (unsigned)frames_, (unsigned)erases_, (unsigned)overruns_,
                  (long long)slowestPassUs_, (long long)slowestDiscoverUs_);
}

#endif  // USE_EPD_PAINTER
