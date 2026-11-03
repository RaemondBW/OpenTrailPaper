// A paint path that runs alongside EPD_Painter's and shares its picture of the glass.
//
// Frames go out one at a time at a constant pass period, newest wins, with black to
// white primed so it erases cleanly. Measured against the driver's own paths on the
// bench, this is what looked best: see investigations/modos-hybrid-update.md.
//
// The driver stays in charge of everything else. Its screenbuffer is the one record
// of what is on the panel, and a frame holds its paint lock, so clear(), dirty-rect
// scrubs and the driver's own paint all work unchanged, in either order, and turning
// this on and off between frames needs no handover.
#pragma once

#include <Arduino.h>

#include "EPD_Painter.h"
#include "scan_core.h"

class EpdScan {
public:
    explicit EpdScan(EPD_Painter& painter) : painter_(painter) {}

    bool begin();

    // Queue a frame from the renderers' 4bpp buffer (low nibble = even x). `levels`
    // maps a nibble to a panel level. Newest wins, so this does not wait for the panel.
    void post(const uint8_t* fb4, const uint8_t* levels);

    // Take the rectangles that differ from this frame to white with the primed erase,
    // for a scrub to finish. The driver's own scrub erases with its plain trains, which
    // is the erase this path exists to replace, and it then records those pixels as
    // white, so what they leave behind is never touched again. Blocks until done.
    void eraseDirty(const uint8_t* fb4, const uint8_t* levels, int tolerance);

    // The same for an exact shape: bit n of `bits` covers byte n of the 4bpp framebuffer
    // (LSB first), so two pixels. Nothing else on the panel moves. Blocks until done.
    void eraseBits(const uint8_t* bits);

    // Everything posted so far is on the glass. Bounded, so a wedged panel can never
    // turn a shutdown into a hang. Returns false if it gave up.
    bool waitIdle(uint32_t timeoutMs);

    // Change the black and white trains, between frames. Not saved: for judging on the
    // glass, with the numbers that win going into epd_scan.cpp. False if out of range.
    bool tune(int drivePasses, int prime);

    void report() const;

private:
    static void taskEntry(void* self);
    void task();
    void drive(int passes);
    void pack(const uint8_t* fb4, const uint8_t* levels);
    int discover(const uint8_t* target);
    void finish(int passes);
    int discoverErase();
    void neutralFrame();
    void buildPlan();

    EPD_Painter& painter_;
    scan::Plan plan_;
    uint8_t drivePasses_ = 0;
    uint8_t prime_ = 0;
    uint8_t* planes_[scan::kPlanes] = {};
    uint32_t* masks_[scan::kPlanes] = {};
    uint8_t* mail_ = nullptr;
    uint8_t* eraseTarget_ = nullptr;
    uint8_t pack_[256] = {};              // a 4bpp byte as two 2-bit levels, even x high
    const uint8_t* packFor_ = nullptr;
    SemaphoreHandle_t mailLock_ = nullptr;
    TaskHandle_t task_ = nullptr;
    volatile bool mailFull_ = false;
    volatile bool eraseWanted_ = false;
    int eraseTolerance_ = 0;
    const uint8_t* eraseBits_ = nullptr;
    volatile bool driving_ = false;

    uint32_t frames_ = 0, erases_ = 0, overruns_ = 0;
    int64_t slowestPassUs_ = 0, slowestDiscoverUs_ = 0;
};
