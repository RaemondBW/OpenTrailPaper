// The device half of hybrid_core.h: a continuous scan at a constant pass period
// for as long as any cohort is on the wire.
#pragma once

#include <Arduino.h>

#include "EPD_Painter.h"
#include "hybrid_core.h"

class EpdHybrid {
public:
    explicit EpdHybrid(EPD_Painter& painter) : painter_(painter) {}

    bool begin();

    // Takes the wire. The driver must be idle, and the glass white and matching
    // its screenbuffer; nothing may call paint() or clear() until disengage().
    void engage();
    // Waits for the drive to finish and hands the wire back. The driver's
    // screenbuffer is stale after this: clear() before painting through it.
    void disengage();

    // Queue a packed 2bpp frame (first pixel in the MSBs). Newest wins, so this
    // never blocks on the panel.
    void post(const uint8_t* frame);
    bool quiet() const { return engaged_ && !mailFull_ && core_->idle(); }

    hybrid::Params& params() { return core_->params; }
    int64_t passUs = 10000;     // the constant pass period every train is dosed at
    // One frame at a time, as the driver does it: the same trains at the same
    // period, with no overlap and so no cancellation. What separates `batch` from
    // `hybrid` is exactly what cohorts contribute.
    bool batch = false;

    void printStats() const;

    // Running totals, plus peaks since the last resetPeaks(): enough to report on
    // one stretch of a run.
    struct Counters {
        uint32_t posted, accepted, refused, passes, overruns, floats, reversed, deferred, settles;
        int64_t slowestPassUs, slowestSubmitUs, worstStartUs, meanScanUs;
        int peakCohorts;
    };
    Counters counters() const;
    void resetPeaks();

private:
    static void taskEntry(void* self);
    void task();
    void takeMail();
    void scanPass();
    void neutralFrame();

    EPD_Painter& painter_;
    hybrid::Core* core_ = nullptr;
    SemaphoreHandle_t mailLock_ = nullptr;
    uint8_t* mail_ = nullptr;
    uint8_t* shadow_ = nullptr;      // the last frame posted; dirty masks diff this
    uint32_t mailDirty_[hybrid::kHeight] = {};
    volatile bool mailFull_ = false;
    int64_t mailSince_ = 0;
    volatile bool engaged_ = false;
    volatile bool parked_ = true;
    bool wireLive_ = false;

    uint32_t passes_ = 0, overruns_ = 0, floats_ = 0, posted_ = 0;
    int64_t scanSumUs_ = 0;
    uint32_t scans_ = 0;
    int64_t maxPassUs_ = 0, maxSubmitUs_ = 0, startDelaySumUs_ = 0, startDelayMaxUs_ = 0;
    uint32_t started_ = 0;
};
