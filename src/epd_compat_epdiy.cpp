// epdc_* panel layer backed by epdiy, for the fallback firmware build.
//
// The port moved the panel behind the epdc_* seam declared in epd_compat.h so
// ui_dashboard.cpp has exactly one code path (see the refresh() comment there for
// what that let us delete). This file keeps env t5s3-pro — the epdiy build that
// shipped as v0.85 — compiling and running against that same seam, so the
// fallback stays alive and CI stays green while the EPD_Painter build is proven
// on hardware.
//
// It is deliberately dumber than the code it replaces. The old path chose
// between DU, GL16 and GC16 per frame and carried ~260 lines of ghost-debt
// accounting to make DU survivable; all of that was compensating for epdiy
// behaviour that EPD_Painter does not have, and reproducing it here would be
// resurrecting the thing the port exists to delete. So every paint is a plain
// GL16: slower than DU (~600 ms vs ~250 ms) and it does not drift, which is the
// right trade for a fallback. If this build ever has to ship again, the DU policy
// is in git history at ea5c697.
//
// epdiy supplies the thirteen drawing functions here, so epd_compat.cpp's own
// rasteriser is compiled out (its top-of-file guard) and only this shim is built.

#if defined(ARDUINO) && !defined(USE_EPD_PAINTER) && !defined(OTP_EMULATOR)

#include "epd_compat.h"

#include <string.h>

#include "config.h"

namespace {

EpdiyHighlevelState g_hl;
bool g_ready = false;

}  // namespace

bool epdc_begin() {
    if (g_ready) return true;
    epd_init(&epd_board_v7, &ED047TC1, EPD_LUT_64K);
    epd_set_vcom(1560);
    g_hl = epd_hl_init(EPD_BUILTIN_WAVEFORM);
    epd_set_rotation(DISPLAY_ROTATION);
    g_ready = true;
    return true;
}

uint8_t* epdc_framebuffer() {
    return g_ready ? epd_hl_get_framebuffer(&g_hl) : nullptr;
}

// epdiy has no notion of a driver-owned grey count; the 4bpp framebuffer is 16
// levels and GL16 renders them. Reported as 16 so callers that branch on it pick
// real greys rather than screentones.
int epdc_grey_levels() { return 16; }

void epdc_paint() {
    if (!g_ready) return;
    epd_poweron();
    epd_hl_update_screen(&g_hl, MODE_GL16, epd_ambient_temperature());
    epd_poweroff();
}

// Nothing to wait for: epd_hl_update_screen() above drives the panel inline and
// has already returned by the time epdc_paint() does. Present so callers that
// must not sleep mid-drive (shutdownDevice) can be backend-agnostic.
void epdc_paint_wait() {}

// Already down — this backend powers the rails on and off around every paint
// (above), which is why the deep-sleep drain the EPD_Painter port introduced was
// never a problem here. Kept so the shutdown path can stay backend-agnostic.
void epdc_power_off_soon() {}
void epdc_power_off_wait() {
    if (!g_ready) return;
    epd_poweroff();   // belt and braces if a future paint path forgets
}

void epdc_clear(int passes) {
    if (!g_ready) return;
    if (passes < 1) passes = 1;
    epd_poweron();
    for (int i = 0; i < passes; ++i) epd_clear();
    epd_poweroff();
    // epd_clear() drives the glass white but leaves back_fb — epdiy's record of
    // what is on the glass — holding the old image, so the next differential
    // update would skip every pixel it believes is already correct and come back
    // partially blank. Reset that record to white.
    //
    // NOT epd_hl_set_all_white(), which whitens front_fb: that is the buffer the
    // renderers draw into, and epdc_clear() must leave the caller's frame intact
    // (shutdownDevice draws the goodbye screen, clears, then paints it). The
    // EPD_Painter implementation has the same contract.
    if (g_hl.back_fb) memset(g_hl.back_fb, 0xFF, (size_t)epd_width() / 2 * epd_height());
}

// epdiy has no dirty-rect scrub (computeDirtyRects is EPD_Painter's), so the
// fallback is the whole-panel clear this backend always did. Keeping the
// signature means the UI never has to know which backend it is on.
void epdc_clear_dirty(int tolerance) {
    (void)tolerance;
    epdc_clear();
}

#endif  // ARDUINO && !USE_EPD_PAINTER
