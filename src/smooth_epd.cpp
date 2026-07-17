#include "smooth_epd.h"

#include <Arduino.h>
#include <epdiy.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "diag.h"

// epdiy low-level LCD primitives. These are exported (non-static) by the
// vendored driver (output_lcd/lcd_driver.c) but only declared in an internal
// header, so we forward-declare exactly the three we need rather than add the
// library's private include path to the build.
extern "C" {
typedef bool (*line_cb_func_t)(void*, uint8_t*);
typedef void (*frame_done_func_t)(void*);
void epd_lcd_line_source_cb(line_cb_func_t cb, void* payload);
void epd_lcd_frame_done_cb(frame_done_func_t cb, void* payload);
void epd_lcd_start_frame();
}

namespace {

// Native source-line width in bytes: 2 bits/pixel, 4 px/byte. The panel is
// always driven in its native 960-wide orientation regardless of the logical
// rotation, so take the larger dimension.
int g_lineBytes = 0;

// Current per-pixel drive code, replicated across the whole line. Two bits per
// pixel: 0b01 = drive toward black, 0b10 = drive toward white, 0b00 = no-op.
// 0x55 = all-black, 0xAA = all-white (four pixels per byte).
volatile uint8_t g_lineCode = 0x00;

SemaphoreHandle_t g_frameDone = nullptr;

// Runs in the LCD DMA/ISR context: must stay in IRAM and touch nothing in
// flash. Fills one native source line with the current uniform drive code.
bool IRAM_ATTR lineCb(void* /*ctx*/, uint8_t* buf) {
    uint8_t code = g_lineCode;
    for (int i = 0; i < g_lineBytes; ++i) buf[i] = code;
    return false;
}

void IRAM_ATTR frameDoneCb(void* /*ctx*/) {
    BaseType_t hp = pdFALSE;
    if (g_frameDone) xSemaphoreGiveFromISR(g_frameDone, &hp);
    if (hp) portYIELD_FROM_ISR();
}

// Drive one uniform field and wait for it to finish (bounded, so a missed
// frame-done interrupt can never wedge the UI task).
void driveField(uint8_t code) {
    g_lineCode = code;
    epd_lcd_start_frame();
    xSemaphoreTake(g_frameDone, pdMS_TO_TICKS(100));
}

}  // namespace

namespace smooth_epd {

void selfTest() {
    int w = epd_width(), h = epd_height();
    int nativeW = w > h ? w : h;   // 960 regardless of rotation
    g_lineBytes = nativeW / 4;

    if (!g_frameDone) g_frameDone = xSemaphoreCreateBinary();
    if (!g_frameDone) { diag::log("smooth: sem alloc failed"); return; }

    diag::log("smooth: self-test start (%d lines, %d bytes/line)", h, g_lineBytes);

    epd_poweron();
    epd_lcd_frame_done_cb(frameDoneCb, nullptr);
    epd_lcd_line_source_cb(lineCb, nullptr);

    // A few dark<->light cycles. ~20 fields per transition is plenty to fully
    // switch the ink so the pulse is unmistakable on-glass.
    const int FIELDS = 20;
    for (int cycle = 0; cycle < 3; ++cycle) {
        for (int i = 0; i < FIELDS; ++i) driveField(0x55);  // toward black
        for (int i = 0; i < FIELDS; ++i) driveField(0xAA);  // toward white
    }

    // Detach our callbacks (epdiy re-installs its own on the next update) and
    // hand the panel back cleanly.
    epd_lcd_line_source_cb(nullptr, nullptr);
    epd_lcd_frame_done_cb(nullptr, nullptr);
    epd_poweroff();
    epd_clear();   // reset to a known white; next UI refresh redraws

    diag::log("smooth: self-test done");
}

}  // namespace smooth_epd
