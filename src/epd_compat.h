#pragma once

// Drop-in replacement for the drawing half of epdiy, so the panel can be driven
// by EPD_Painter instead.
//
// WHY THIS EXISTS
// epdiy and EPD_Painter cannot coexist: epd_renderer_init() asserts a non-NULL
// board, calls board->init() and epd_control_reg_init(), and claims LCD_CAM. But
// epdiy's *drawing* functions are pure software — they only touch a caller-owned
// framebuffer. So we keep epdiy's HEADERS for the types (EpdRect, EpdFont,
// EpdFontProperties, rotation enums), do not compile any epdiy source, and
// implement the 13 drawing functions our renderers actually call right here.
//
// The payoff: ui_render.cpp (891 lines), map_view.cpp (469 lines) and all four
// font data headers compile COMPLETELY UNCHANGED, and tools/preview keeps
// working. Only the panel layer in ui_dashboard.cpp changes.
//
// The framebuffer stays in epdiy's exact 4bpp format — 2 pixels per byte, low
// nibble = even x, 0x0 black .. 0xF white, 480 bytes per native row. Keeping
// that format is what makes the renderers portable; conversion to
// EPD_Painter's 8bpp level buffer happens once, at paint time (see
// epdc_paint()), not throughout the drawing code.
//
// Implementations here are written from scratch (Bresenham, scanline fills,
// glyph blitting) rather than copied from epdiy, which is LGPL-3.0 while this
// project is Apache-2.0.

#include <stdint.h>

#include "epdiy.h"   // TYPES ONLY — no epdiy source is compiled or linked

// ---------------------------------------------------------------------------
// Panel layer (the part that replaces epd_hl_* / epd_init / epd_poweron)
// ---------------------------------------------------------------------------

// Bring up EPD_Painter and allocate the 4bpp framebuffer plus the 8bpp level
// buffer paint() consumes. Returns false if either allocation or the driver
// fails. Sets the rotation our UI is laid out for.
bool epdc_begin();

// The 4bpp framebuffer the renderers draw into. Stable for the process
// lifetime; never freed.
uint8_t* epdc_framebuffer();

// Push the framebuffer to the panel. Converts 4bpp -> EPD_Painter levels and
// calls paint(), whose own delta engine drives only what changed — so there is
// no need for the dirty-rect bookkeeping the epdiy path required.
void epdc_paint();

// Block until the panel has actually finished being driven.
//
// epdc_paint() is ASYNCHRONOUS on the EPD_Painter backend: paint() hands the
// buffer to the driver's epd_paint task and returns as soon as that task has
// picked it up ("wait until this buffer has been picked up by the paint loop"),
// while the rows are still being clocked out. Normally that is exactly what we
// want — the UI task gets on with the next frame. It is wrong only when the
// very next thing we do stops the CPU: deep sleep mid-drive leaves a partially
// written image on the glass, which is what the shutdown screen was showing.
//
// No-op on the epdiy backend, whose epd_hl_update_screen() already blocks.
void epdc_paint_wait();

// Cut the panel's high-voltage rails (VPOS/VNEG/VGH/VGL/VCOM) before deep sleep.
//
// Without this the TPS65185 keeps generating rails for the whole sleep. The
// EPD_Painter backend does drop them on its own — but on a 5-second idle timer
// run by its own `panel_idle_off` task, and shutdownDevice() stops the CPU long
// inside that window, so the timer never fires. The enables are latched in the
// XL9555 expander, which sits on the always-on 3V3, so nothing else drops them
// either. (The epdiy backend powered the rails down around every paint, which is
// why this only became a problem with the EPD_Painter port.)
//
// Two calls because of how that timer works: it is armed at paint time, so the
// countdown has to be shortened BEFORE the last paint, and waited for after it.
//
//   epdc_power_off_soon();   // then paint the farewell screen
//   epdc_paint_wait();
//   epdc_power_off_wait();
//
// powerOff() itself is private to the driver, and reproducing it here would mean
// duplicating its TPS-register-then-expander sequencing — so this drives the
// driver's own path instead of reimplementing it.
//
// Safe for the image: e-paper holds its last frame with no power at all, which is
// the whole premise of the farewell screen.
void epdc_power_off_soon();
void epdc_power_off_wait();

// How long the panel may sit unpainted before the driver drops its rails, in
// seconds.
//
// Do NOT pass 0 meaning "never": the driver re-arms its countdown to this
// value on every paint and powers ON whenever it finds the countdown at zero,
// so 0 makes every paint take the power-up path rather than none of them.
// Pass a value longer than the gap you need to cover.
//
// This exists because of what the driver does at the far end of that timer.
// PanelPowerGuard re-arms a counter to `_idle_timeout_s` on every paint, a 1 Hz
// task decrements it, and at zero it powers the panel OFF; the next paint then
// has to bring the rails back up inline. Boot has one long gap with no paint —
// the SD tile scan, ~5 s and growing with the card — so whether the power-off
// lands inside that gap is decided by where a free-running 1 Hz tick happens to
// fall. When it did, the paint that followed wedged with interrupts disabled
// and the interrupt watchdog reset the device: a boot loop that appeared once
// the tile count grew, was intermittent (seven resets, then a clean boot), and
// looked for all the world like a fault in the map code it happened to follow.
void epdc_set_idle_timeout(int seconds);

// Drive the whole panel to white. `passes` > 1 repeats it: e-paper keeps its
// image through power-off, so the first boot after a different firmware needs
// several passes to shift what is physically on the glass (one was not enough,
// verified on hardware).
//
// Leaves the framebuffer untouched — it resets what is on the GLASS and the
// driver's record of it, not what the renderers have drawn. So the sequence
// "draw, clear, paint" puts the drawn frame on a scrubbed panel, which is what
// the shutdown screen wants.
void epdc_clear(int passes = 1);

// Scrub ONLY the regions that differ between the glass and the frame about to
// be painted, instead of the whole panel.
//
// WHY THIS EXISTS. The driver's delta engine never drives a black pixel black
// again — the dark plane is masked to pixels the screenbuffer records as white
// (EPD_Painter.S, epd_painter_ink_dual) — so residue never comes from
// over-driving. It comes from the other direction: the same kernel records
// "lightened pixels ... as white" the moment it issues one light pulse. If deep
// black does not fully erase in that pulse, the leftover grey is now INVISIBLE
// to the driver — its model says white, so no later frame will ever drive those
// pixels again. That is the haze the dashboard used to leave under the map's
// streets, and why only a clear fixed it: a clear is the one operation that
// resyncs the driver's model to the glass.
//
// A whole-panel clear was a blunt way to buy that resync. This clears the
// changed rectangles only (computeDirtyRects + a SOFT partial clear).
//
// `tolerance` is how many wasted pixels of clean area may sit inside one
// rectangle before it is split — 0 gives tight rects, large values collapse
// toward one full-screen rect. It matters because the driver caps the scan at
// 32 rectangles and stops scanning once it hits that ceiling, leaving anything
// below unscrubbed; merging generously keeps the count under it.
//
// Falls back to a whole-panel clear on the epdiy backend, which has no
// dirty-rect machinery.
void epdc_clear_dirty(int tolerance = 0);

// Number of grey levels the driver is currently using (4 by default). Exposed
// so the UI can decide between real greys and screentones.
// Ink height of a digit ('0') in this font. Use this, not the height reported
// by epd_get_string_rect, to decide whether a number fits a box: that rect is
// the background band (ascender..descender), which is far taller than the
// digits and rejects faces that would fit comfortably.
int epdc_digit_height(const EpdFont* font);

int epdc_grey_levels();

// ---------------------------------------------------------------------------
// The epdiy drawing API we implement. Signatures match epdiy exactly.
// ---------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

int epd_width();
int epd_height();
int epd_rotated_display_width();
int epd_rotated_display_height();
void epd_set_rotation(enum EpdRotation rotation);
enum EpdRotation epd_get_rotation();

void epd_draw_pixel(int x, int y, uint8_t color, uint8_t* framebuffer);
void epd_draw_hline(int x, int y, int length, uint8_t color, uint8_t* framebuffer);
void epd_draw_vline(int x, int y, int length, uint8_t color, uint8_t* framebuffer);
void epd_draw_line(int x0, int y0, int x1, int y1, uint8_t color, uint8_t* framebuffer);
void epd_fill_rect(EpdRect rect, uint8_t color, uint8_t* framebuffer);
void epd_draw_rect(EpdRect rect, uint8_t color, uint8_t* framebuffer);
void epd_draw_circle(int x0, int y0, int r, uint8_t color, uint8_t* framebuffer);
void epd_fill_circle(int x0, int y0, int r, uint8_t color, uint8_t* framebuffer);
void epd_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
                       uint8_t color, uint8_t* framebuffer);

EpdFontProperties epd_font_properties_default();
// Returns EpdDrawError to match epdiy's declaration exactly; always SUCCESS
// here, since there is no hardware to fail.
enum EpdDrawError epd_write_string(const EpdFont* font, const char* string,
                                   int* cursor_x, int* cursor_y,
                                   uint8_t* framebuffer,
                                   const EpdFontProperties* properties);
EpdRect epd_get_string_rect(const EpdFont* font, const char* string, int x, int y,
                            int margin, const EpdFontProperties* properties);

#ifdef __cplusplus
}
#endif
