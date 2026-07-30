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

// Drive the whole panel to white. `passes` > 1 repeats it: e-paper keeps its
// image through power-off, so the first boot after a different firmware needs
// several passes to shift what is physically on the glass (one was not enough,
// verified on hardware).
void epdc_clear(int passes = 1);

// Number of grey levels the driver is currently using (4 by default). Exposed
// so the UI can decide between real greys and screentones.
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
