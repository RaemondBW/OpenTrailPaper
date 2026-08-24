// The EMULATOR panel half of epd_compat.h — see investigations/web-emulator.md.
//
// Instead of driving glass, epdc_paint() streams the 4bpp framebuffer out
// UART1 as an RLE frame, and the web page draws it on a canvas. The drawing
// half (all thirteen epdiy-signature functions) is shared with the other
// backends in epd_compat.cpp; only this file knows there is no panel.
//
// Wire protocol (matches web/emulator.js):
//   frame:  0xF5 'F' [u16 seq LE] [u32 rleLen LE] <rle bytes> 0xF6
//           rle = [count u8][value u8] pairs over the 4bpp buffer,
//                 540*960/2 = 259200 bytes expanded
//   clear:  0xF5 'C' 0xF6      (drive-to-white; canvas whitens, fb untouched)
// Input events arrive on the same UART and are consumed by emu_input.cpp.

#if defined(ARDUINO) && defined(OTP_EMULATOR)

#include "epd_compat.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "diag.h"

namespace {

constexpr int W = 540, H = 960;              // rotated, what the UI draws in
constexpr size_t FB_BYTES = 540UL * 960UL / 2;

uint8_t* fb = nullptr;
uint16_t frameSeq = 0;

void putU16(uint16_t v) { Serial1.write(v & 0xFF); Serial1.write(v >> 8); }
void putU32(uint32_t v) { putU16(v & 0xFFFF); putU16(v >> 16); }

}  // namespace

bool epdc_begin() {
    // UART1 is the frame/event wire. Pins are irrelevant under QEMU — the
    // peripheral index is what maps to the host's second -serial option — but
    // Serial1.begin still wants legal ones.
    Serial1.begin(921600, SERIAL_8N1, 17, 18);

    // PSRAM first, like the real backends: the framebuffer is the exact
    // allocation the shipping firmware makes, and QEMU models the PSRAM size.
    fb = (uint8_t*)heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
    if (!fb) fb = (uint8_t*)malloc(FB_BYTES);
    if (!fb) return false;
    memset(fb, 0xFF, FB_BYTES);   // white, matching a powered-on clean panel

    epd_set_rotation(EPD_ROT_PORTRAIT);
    diag::log("emu: panel backend up (%dx%d, frames on UART1)", W, H);
    return true;
}

uint8_t* epdc_framebuffer() { return fb; }

void epdc_paint() {
    if (!fb) return;
    // RLE in one pass straight to the UART: the driver task QEMU gives the
    // UART is effectively host-speed, and a dashboard frame is runs of white
    // almost everywhere, so this lands in tens of KB.
    Serial1.write(0xF5);
    Serial1.write('F');
    putU16(frameSeq++);

    // First pass to size the payload (the header carries rleLen so the web
    // side can frame the stream without lookahead).
    uint32_t rleLen = 0;
    for (size_t i = 0; i < FB_BYTES;) {
        uint8_t v = fb[i];
        size_t run = 1;
        while (run < 255 && i + run < FB_BYTES && fb[i + run] == v) run++;
        i += run;
        rleLen += 2;
    }
    putU32(rleLen);
    for (size_t i = 0; i < FB_BYTES;) {
        uint8_t v = fb[i];
        size_t run = 1;
        while (run < 255 && i + run < FB_BYTES && fb[i + run] == v) run++;
        Serial1.write((uint8_t)run);
        Serial1.write(v);
        i += run;
    }
    Serial1.write(0xF6);
}

// Streaming in epdc_paint() is synchronous, so the async-drive choreography
// the real panel needs is all no-ops here.
void epdc_paint_wait() {}
void epdc_power_off_soon() {}
void epdc_power_off_wait() {}

void epdc_clear(int passes) {
    (void)passes;   // one message regardless — a canvas whitens in one step
    Serial1.write(0xF5);
    Serial1.write('C');
    Serial1.write(0xF6);
}

void epdc_clear_dirty(int tolerance) {
    (void)tolerance;
    // The residue this scrubs is a physics artefact of real ink. A canvas has
    // no invisible-grey failure mode, so resyncing is free and unnecessary.
}

// Match the shipping EPD_Painter default so the UI makes the same
// screentone-vs-grey decisions the device makes.
int epdc_grey_levels() { return 4; }

#endif  // ARDUINO && OTP_EMULATOR
