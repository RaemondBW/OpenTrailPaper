// epdiy-compatible software rasteriser. See epd_compat.h for why this exists.
//
// Written from scratch (standard Bresenham / scanline algorithms and a 4bpp
// glyph blit) rather than adapted from epdiy, which is LGPL-3.0 while this
// project is Apache-2.0.

#include "epd_compat.h"

#include <stdlib.h>
#include <string.h>

// Compiled for the EPD_Painter device build and for the host preview harness.
// The epdiy device build (env t5s3-pro) gets its drawing functions from epdiy
// itself, so compiling ours there would be a duplicate definition of all
// thirteen; its epdc_* panel shim lives in epd_compat_epdiy.cpp instead.
#if !defined(ARDUINO) || defined(USE_EPD_PAINTER)

namespace {

// Native panel geometry. The framebuffer is always in NATIVE orientation;
// rotation is applied per-pixel on the way in, exactly as epdiy does, so our
// layout code keeps working in rotated (540x960) coordinates.
constexpr int kNativeW = 960;
constexpr int kNativeH = 540;
constexpr int kBytesPerRow = kNativeW / 2;   // 4bpp: 2 px per byte

enum EpdRotation g_rotation = EPD_ROT_LANDSCAPE;

// Rotate a rotated-space coordinate into native space. Must match epdiy's
// _rotate() exactly or every screen shifts.
inline bool rotateToNative(int x, int y, int& nx, int& ny) {
    switch (g_rotation) {
        case EPD_ROT_LANDSCAPE:
            nx = x; ny = y;
            break;
        case EPD_ROT_PORTRAIT:
            nx = kNativeW - y - 1; ny = x;
            break;
        case EPD_ROT_INVERTED_LANDSCAPE:
            nx = kNativeW - x - 1; ny = kNativeH - y - 1;
            break;
        case EPD_ROT_INVERTED_PORTRAIT:
            nx = y; ny = kNativeH - x - 1;
            break;
        default:
            nx = x; ny = y;
            break;
    }
    return nx >= 0 && nx < kNativeW && ny >= 0 && ny < kNativeH;
}

// Write one native-space pixel. `color` is an epdiy 8-bit value; only the high
// nibble carries the 4bpp level (0x00 black .. 0xF0/0xFF white), matching how
// epdiy's callers pass 0x00 / 0xFF.
inline void putNative(int nx, int ny, uint8_t color, uint8_t* fb) {
    uint8_t* p = &fb[(size_t)ny * kBytesPerRow + (nx >> 1)];
    if (nx & 1) *p = (*p & 0x0F) | (color & 0xF0);
    else        *p = (*p & 0xF0) | (color >> 4);
}

}  // namespace

// ---------------------------------------------------------------------------
// Geometry / rotation
// ---------------------------------------------------------------------------

int epd_width()  { return kNativeW; }
int epd_height() { return kNativeH; }

int epd_rotated_display_width() {
    return (g_rotation == EPD_ROT_PORTRAIT || g_rotation == EPD_ROT_INVERTED_PORTRAIT)
               ? kNativeH : kNativeW;
}
int epd_rotated_display_height() {
    return (g_rotation == EPD_ROT_PORTRAIT || g_rotation == EPD_ROT_INVERTED_PORTRAIT)
               ? kNativeW : kNativeH;
}

void epd_set_rotation(enum EpdRotation rotation) { g_rotation = rotation; }
enum EpdRotation epd_get_rotation() { return g_rotation; }

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

void epd_draw_pixel(int x, int y, uint8_t color, uint8_t* fb) {
    int nx, ny;
    if (rotateToNative(x, y, nx, ny)) putNative(nx, ny, color, fb);
}

void epd_draw_hline(int x, int y, int length, uint8_t color, uint8_t* fb) {
    for (int i = 0; i < length; ++i) epd_draw_pixel(x + i, y, color, fb);
}

void epd_draw_vline(int x, int y, int length, uint8_t color, uint8_t* fb) {
    for (int i = 0; i < length; ++i) epd_draw_pixel(x, y + i, color, fb);
}

// Bresenham, stepping along whichever axis is longer and always left-to-right on
// that axis.
//
// The tie-breaking here is deliberate, not incidental: the symmetric two-error
// form this used at first draws a *valid* line that picks different pixels on
// roughly half of all diagonals, which showed up as ~90 stray pixels per map
// screen against the epdiy baseline. Map polylines and the turn arrow are drawn
// with this, so matching the shipped rasterisation is what keeps the port
// invisible. Axis-aligned cases go to the span helpers, again as epdiy does — an
// exactly-horizontal line must not depend on the DDA rounding at all.
void epd_draw_line(int x0, int y0, int x1, int y1, uint8_t color, uint8_t* fb) {
    if (x0 == x1) {
        if (y0 > y1) { const int t = y0; y0 = y1; y1 = t; }
        epd_draw_vline(x0, y0, y1 - y0 + 1, color, fb);
        return;
    }
    if (y0 == y1) {
        if (x0 > x1) { const int t = x0; x0 = x1; x1 = t; }
        epd_draw_hline(x0, y0, x1 - x0 + 1, color, fb);
        return;
    }

    const bool steep = abs(y1 - y0) > abs(x1 - x0);
    if (steep) {
        int t = x0; x0 = y0; y0 = t;
        t = x1; x1 = y1; y1 = t;
    }
    if (x0 > x1) {
        int t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }

    const int dx = x1 - x0, dy = abs(y1 - y0);
    const int ystep = y0 < y1 ? 1 : -1;
    int err = dx / 2;
    for (; x0 <= x1; ++x0) {
        if (steep) epd_draw_pixel(y0, x0, color, fb);
        else       epd_draw_pixel(x0, y0, color, fb);
        err -= dy;
        if (err < 0) { y0 += ystep; err += dx; }
    }
}

void epd_fill_rect(EpdRect rect, uint8_t color, uint8_t* fb) {
    for (int y = 0; y < rect.height; ++y)
        epd_draw_hline(rect.x, rect.y + y, rect.width, color, fb);
}

void epd_draw_rect(EpdRect rect, uint8_t color, uint8_t* fb) {
    if (rect.width <= 0 || rect.height <= 0) return;
    epd_draw_hline(rect.x, rect.y, rect.width, color, fb);
    epd_draw_hline(rect.x, rect.y + rect.height - 1, rect.width, color, fb);
    epd_draw_vline(rect.x, rect.y, rect.height, color, fb);
    epd_draw_vline(rect.x + rect.width - 1, rect.y, rect.height, color, fb);
}

// Midpoint circle, outline.
void epd_draw_circle(int x0, int y0, int r, uint8_t color, uint8_t* fb) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        epd_draw_pixel(x0 + x, y0 + y, color, fb);
        epd_draw_pixel(x0 + y, y0 + x, color, fb);
        epd_draw_pixel(x0 - y, y0 + x, color, fb);
        epd_draw_pixel(x0 - x, y0 + y, color, fb);
        epd_draw_pixel(x0 - x, y0 - y, color, fb);
        epd_draw_pixel(x0 - y, y0 - x, color, fb);
        epd_draw_pixel(x0 + y, y0 - x, color, fb);
        epd_draw_pixel(x0 + x, y0 - y, color, fb);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

// Filled circle: one horizontal span per row, its half-width taken from the
// midpoint outline above.
//
// Deriving the spans from the same recurrence as epd_draw_circle — rather than
// from the exact disc dx^2 + dy^2 <= r^2, which is what this did first — matters
// because it is what makes the result pixel-identical to epdiy's fill for every
// radius 1..40 (verified exhaustively). The exact disc is a rounder circle by any
// mathematical measure and about 12 pixels different at r=4, which is enough to
// make the GPS signal dots in the status bar visibly thinner than they have been.
// Matching the shipped look wins over matching the ideal one.
void epd_fill_circle(int x0, int y0, int r, uint8_t color, uint8_t* fb) {
    if (r < 0) return;
    // Half-width per row, indexed by dy + r. Stack-resident: this runs per dot in
    // the map render loop, and a malloc/free pair per circle there is not worth
    // it. 128 covers every radius the UI uses (the largest is the compass ring);
    // anything bigger falls back to the exact disc rather than growing the frame.
    constexpr int kMaxR = 128;
    if (r > kMaxR) {
        for (int dy = -r; dy <= r; ++dy) {
            int dx = 0;
            while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;
            epd_draw_hline(x0 - dx, y0 + dy, 2 * dx + 1, color, fb);
        }
        return;
    }
    int half[2 * kMaxR + 1] = {0};
    auto mark = [&](int dx, int dy) {
        const int i = dy + r;
        if (i < 0 || i > 2 * r) return;
        const int ax = dx < 0 ? -dx : dx;
        if (ax > half[i]) half[i] = ax;
    };

    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        mark(x, y);  mark(y, x);  mark(-y, x);  mark(-x, y);
        mark(-x, -y); mark(-y, -x); mark(y, -x); mark(x, -y);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }

    for (int dy = -r; dy <= r; ++dy) {
        const int hx = half[dy + r];
        epd_draw_hline(x0 - hx, y0 + dy, 2 * hx + 1, color, fb);
    }
}

// Scanline triangle fill. Vertices are sorted by y, then each scanline spans
// between the two active edges. Long edge is v0->v2 throughout; the short edge
// switches from v0->v1 to v1->v2 at the middle vertex.
void epd_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
                       uint8_t color, uint8_t* fb) {
    auto swapPts = [](int& ax, int& ay, int& bx, int& by) {
        int t = ax; ax = bx; bx = t;
        t = ay; ay = by; by = t;
    };
    if (y0 > y1) swapPts(x0, y0, x1, y1);
    if (y1 > y2) swapPts(x1, y1, x2, y2);
    if (y0 > y1) swapPts(x0, y0, x1, y1);

    if (y0 == y2) {   // degenerate: a horizontal line
        int lo = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
        int hi = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
        epd_draw_hline(lo, y0, hi - lo + 1, color, fb);
        return;
    }

    for (int y = y0; y <= y2; ++y) {
        // Long edge v0 -> v2
        const int a = x0 + (int)((int64_t)(x2 - x0) * (y - y0) / (y2 - y0));
        // Short edge: v0 -> v1 above the middle vertex, v1 -> v2 below it
        int b;
        if (y < y1) {
            b = (y1 == y0) ? x1
                           : x0 + (int)((int64_t)(x1 - x0) * (y - y0) / (y1 - y0));
        } else {
            b = (y2 == y1) ? x1
                           : x1 + (int)((int64_t)(x2 - x1) * (y - y1) / (y2 - y1));
        }
        const int lo = a < b ? a : b;
        const int hi = a < b ? b : a;
        epd_draw_hline(lo, y, hi - lo + 1, color, fb);
    }
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

EpdFontProperties epd_font_properties_default() {
    EpdFontProperties props;
    props.fg_color = 0;          // 4bpp level: 0 = black
    props.bg_color = 15;         // 15 = white
    props.fallback_glyph = 0;
    props.flags = EPD_DRAW_ALIGN_LEFT;
    return props;
}

namespace {

const EpdGlyph* findGlyph(const EpdFont* font, uint32_t cp) {
    if (!font) return nullptr;
    for (uint32_t i = 0; i < font->interval_count; ++i) {
        const EpdUnicodeInterval& iv = font->intervals[i];
        if (cp >= iv.first && cp <= iv.last)
            return &font->glyph[iv.offset + (cp - iv.first)];
    }
    return nullptr;
}

// Minimal UTF-8 decode. Our strings are ASCII plus the '·' separator, so this
// only needs to handle 1- and 2-byte sequences correctly and skip the rest.
uint32_t nextCodePoint(const char** s) {
    const uint8_t* p = (const uint8_t*)*s;
    uint32_t cp = *p;
    if (cp < 0x80) { *s += 1; return cp; }
    if ((cp & 0xE0) == 0xC0) { cp = ((cp & 0x1F) << 6) | (p[1] & 0x3F); *s += 2; return cp; }
    if ((cp & 0xF0) == 0xE0) {
        cp = ((cp & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        *s += 3; return cp;
    }
    *s += 1;
    return cp;
}

// Accumulate the bounding box of a string, mirroring epdiy's get_char_bounds /
// epd_get_text_bounds exactly. Getting this wrong shifts every centred and
// right-aligned label on every screen, so it is worth the fidelity.
//
// The subtlety is that the two callers want DIFFERENT boxes, which is what
// `background` selects — epd_get_string_rect forces EPD_DRAW_BACKGROUND on and
// so measures the full advance box (pen start to pen end, ascender to
// descender), while alignment in epd_write_string measures the INK box (leftmost
// to rightmost lit pixel). Those differ by the last glyph's right side bearing,
// typically 1-3 px: enough that right-aligned text measured the advance way
// stops sitting flush against its anchor.
struct TextBounds { int minx, miny, maxx, maxy, penEnd; };

TextBounds measure(const EpdFont* font, const char* s, int x, int y,
                   bool background) {
    TextBounds b{100000, 100000, -1, -1, x};
    int px = x;
    while (*s) {
        const uint32_t cp = nextCodePoint(&s);
        const EpdGlyph* g = findGlyph(font, cp);
        if (!g) continue;
        const int x1 = px + g->left;
        const int y1 = y + g->top - g->height;
        const int x2 = x1 + g->width;
        const int y2 = y1 + g->height;
        if (background) {
            if (px < b.minx) b.minx = px;
            if (x1 < b.minx) b.minx = x1;
            const int adv = px + g->advance_x;
            if (adv > b.maxx) b.maxx = adv;
            if (x2 > b.maxx) b.maxx = x2;
            if (y + font->descender < b.miny) b.miny = y + font->descender;
            if (y1 < b.miny) b.miny = y1;
            if (y + font->ascender > b.maxy) b.maxy = y + font->ascender;
            if (y2 > b.maxy) b.maxy = y2;
        } else {
            if (x1 < b.minx) b.minx = x1;
            if (y1 < b.miny) b.miny = y1;
            if (x2 > b.maxx) b.maxx = x2;
            if (y2 > b.maxy) b.maxy = y2;
        }
        px += g->advance_x;
    }
    b.penEnd = px;
    return b;
}

// The width alignment shifts by: epd_get_text_bounds' `w`.
int alignWidth(const EpdFont* font, const char* s) {
    if (!s || !*s) return 0;
    const TextBounds b = measure(font, s, 0, 0, /*background=*/false);
    if (b.maxx < 0) return 0;                  // nothing drawable
    const int x1 = 0 < b.minx ? 0 : b.minx;    // min(original_x, minx)
    return b.maxx - x1;
}

// Blit one glyph. The bitmap is 4bpp, 2 pixels per byte, low nibble first, each
// nibble an intensity 0..15 (0 = leave background alone).
//
// By default we THRESHOLD that intensity to fully on/off rather than blending it
// toward the background. The bundled fonts are anti-aliased — measured on
// ArialBold_14, ~20% of a glyph's lit pixels sit at intermediate levels — and
// under epdiy those edge pixels were an active problem: DU is strictly 1-bit, so
// every text update drove them toward black or white with no stable target, which
// was the last source of the grey drift (investigations/display-ghosting.md).
// Flattening them made text 1-bit, and that is what has been shipping.
//
// EPD_Painter removes the reason for that constraint, so the blend is available
// again behind EPDC_TEXT_ANTIALIAS. Two things to know before turning it on: the
// driver runs 4 grey levels by default, so 16-level AA collapses to 4 and edges
// go chunky rather than smooth; and the tools/preview diff against the epdiy
// baseline is only zero WITH it on — that is how the geometry above was verified
// (see the same file). Left off because crisp 1-bit reads better than 4-level AA
// at 14-20 px, not because it is unsafe.
void blitGlyph(const EpdFont* font, const EpdGlyph* g, int* cursorX, int cursorY,
               uint8_t fgLevel, uint8_t bgLevel, uint8_t* fb) {
    const int byteWidth = (g->width + 1) / 2;
    const uint8_t* bmp = &font->bitmap[g->data_offset];

#ifdef EPDC_TEXT_ANTIALIAS
    // epdiy's ramp, reproduced exactly: level = bg + v*(fg-bg)/15, clamped.
    uint8_t ramp[16];
    for (int v = 0; v < 16; ++v) {
        int lv = (int)bgLevel + v * ((int)fgLevel - (int)bgLevel) / 15;
        ramp[v] = (uint8_t)(lv < 0 ? 0 : (lv > 15 ? 15 : lv));
    }
#else
    (void)bgLevel;
    const uint8_t color = (uint8_t)(fgLevel << 4);
#endif

    for (int y = 0; y < g->height; ++y) {
        const int py = cursorY - g->top + y;
        for (int x = 0; x < g->width; ++x) {
            const uint8_t byte = bmp[y * byteWidth + (x >> 1)];
            const uint8_t v = (x & 1) ? (byte >> 4) : (byte & 0x0F);
#ifdef EPDC_TEXT_ANTIALIAS
            if (v) epd_draw_pixel(*cursorX + g->left + x, py,
                                  (uint8_t)(ramp[v] << 4), fb);
#else
            if (v >= 8) epd_draw_pixel(*cursorX + g->left + x, py, color, fb);
#endif
        }
    }
    *cursorX += g->advance_x;
}

}  // namespace

enum EpdDrawError epd_write_string(const EpdFont* font, const char* string,
                                   int* cursor_x, int* cursor_y, uint8_t* fb,
                                   const EpdFontProperties* properties) {
    if (!font || !string || !cursor_x || !cursor_y) return EPD_DRAW_STRING_INVALID;
    const EpdFontProperties props = properties ? *properties
                                               : epd_font_properties_default();

    // Alignment: epdiy treats cursor_x as the anchor and shifts by the string
    // width for centre/right. Our layouts rely on this heavily.
    //
    // Mask on the three ALIGN bits by name. They are 0x2/0x4/0x8 — masking 0x3,
    // as this did at first, catches only ALIGN_LEFT and silently left-aligns
    // every right- and centre-aligned string in the UI. It looked plausible on
    // most screens and put the battery percentage on top of the battery icon;
    // the host preview diff is what caught it.
    int x = *cursor_x;
    const int align = props.flags &
        (EPD_DRAW_ALIGN_LEFT | EPD_DRAW_ALIGN_RIGHT | EPD_DRAW_ALIGN_CENTER);
    // Mutually exclusive, as in epdiy: more than one bit set is a caller bug.
    if (align & (align - 1)) return EPD_DRAW_INVALID_FONT_FLAGS;
    if (align == EPD_DRAW_ALIGN_CENTER)     x -= alignWidth(font, string) / 2;
    else if (align == EPD_DRAW_ALIGN_RIGHT) x -= alignWidth(font, string);

    const char* s = string;
    while (*s) {
        const uint32_t cp = nextCodePoint(&s);
        const EpdGlyph* g = findGlyph(font, cp);
        if (!g) g = findGlyph(font, props.fallback_glyph);
        if (!g) continue;
        blitGlyph(font, g, &x, *cursor_y, props.fg_color, props.bg_color, fb);
    }
    *cursor_x = x;
    return EPD_DRAW_SUCCESS;
}

// Ink height of a digit in this font — the '0' glyph's bitmap height.
//
// The only honest answer to "how tall will this number be". epd_get_string_rect
// cannot tell you: epdiy forces EPD_DRAW_BACKGROUND there, so its height is the
// full ascender-to-descender band (103 px for Impact_40, whose digits are 58).
// Sizing a value against that band rejects the big face in cells it fits fine,
// and the alternative — a hardcoded cap height per font — is the guess that put
// numbers through their own captions twice already.
int epdc_digit_height(const EpdFont* font) {
    const EpdGlyph* g = findGlyph(font, '0');
    return g ? g->height : (font ? font->ascender : 0);
}

EpdRect epd_get_string_rect(const EpdFont* font, const char* string, int x, int y,
                            int margin, const EpdFontProperties* properties) {
    (void)properties;   // epdiy forces EPD_DRAW_BACKGROUND on here regardless
    EpdRect r = {(uint16_t)x, (uint16_t)y, 0, 0};
    if (!font || !string || !*string) return r;

    // epdiy measures from y + ascender, and reports width relative to the
    // caller's x (not to minx) — so a glyph with negative left bearing makes the
    // rect start left of x. Kept identical because ui_render's textWidth() is
    // just this .width, and it drives every fits-in-the-box decision.
    const TextBounds b = measure(font, string, x, y + font->ascender,
                                 /*background=*/true);
    if (b.maxx < 0) return r;
    r.width  = (uint16_t)(b.maxx - x + 2 * margin);
    r.height = (uint16_t)(b.maxy - b.miny + 2 * margin);
    return r;
}

// ---------------------------------------------------------------------------
// Panel layer — EPD_Painter
// ---------------------------------------------------------------------------
//
// Only compiled for the device. The host preview harness links the rasteriser
// above and writes PNGs itself, so it must not pull in EPD_Painter or ESP-IDF.
#ifdef ARDUINO

#include <Arduino.h>
#include <Wire.h>
#include <esp_heap_caps.h>

#include "EPD_Painter_presets.h"
#include "EPD_Painter.h"

#include "config.h"

namespace {

EPD_Painter* g_painter = nullptr;
uint8_t* g_fb4 = nullptr;      // 4bpp, epdiy layout — what the renderers draw into
uint8_t* g_levels = nullptr;   // 8bpp level indices — what paint() consumes

// 4bpp nibble (0 black .. 15 white)  ->  EPD_Painter level (0 white .. 3 black).
// Built once so the per-pixel conversion is a table lookup. Note the inversion:
// epdiy uses luminance, EPD_Painter uses level indices, and getting this
// backwards renders everything inverted (it did, on the first hardware run).
uint8_t g_lut[16];

void buildLut() {
    for (int v = 0; v < 16; ++v) {
        // 15 (white) -> 0, 0 (black) -> 3, with the mid values landing on the
        // two intermediate levels. Rounded so 0x0/0xF map exactly to 3/0.
        g_lut[v] = (uint8_t)(((15 - v) * 3 + 7) / 15);
    }
}

}  // namespace

bool epdc_begin() {
    if (g_painter) return true;

    const size_t fb4Size = (size_t)kBytesPerRow * kNativeH;             // 259200
    const size_t lvlSize = (size_t)kNativeW * kNativeH;                 // 518400

    // Report the heap before we start. EPD_Painter wants a substantial chunk of
    // INTERNAL, DMA-capable RAM (two scan-line DMA buffers, a 129600-byte packed
    // fast buffer, and the decision-engine sweep tables), and unlike the
    // standalone eval app this runs after NimBLE, the GPS service and the SD
    // stack have already taken theirs. Several of its failure paths return false
    // without logging anything, so if begin() fails these two numbers are the
    // first thing worth looking at.
    Serial.printf("[epdc] heap before: internal=%u (largest %u) psram=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    g_fb4 = (uint8_t*)heap_caps_malloc(fb4Size, MALLOC_CAP_SPIRAM);
    g_levels = (uint8_t*)heap_caps_aligned_alloc(16, lvlSize, MALLOC_CAP_SPIRAM);
    if (!g_fb4 || !g_levels) {
        Serial.printf("[epdc] PSRAM alloc failed: fb4=%p levels=%p\n",
                      (void*)g_fb4, (void*)g_levels);
        return false;
    }
    memset(g_fb4, 0xFF, fb4Size);      // white
    memset(g_levels, 0, lvlSize);      // level 0 = white

    buildLut();

    // Hand the driver OUR Wire. Not a tidiness point — without this the port
    // hangs on boot, and this was the hang.
    //
    // EPD_Painter::begin() does `if (i2c.wire == nullptr) new TwoWire(0)`, and
    // TwoWire(0) is the very peripheral main.cpp already brought up with
    // Wire.begin(BOARD_SDA, BOARD_SCL). Arduino's TwoWire::begin() then takes
    //     if (i2cIsInit(num)) { started = true; goto end; }
    // and that goto jumps straight over allocateWireBuffer(), so the second
    // instance is left with txBuffer/rxBuffer == NULL while reporting success.
    // Every write() on it returns 0 ("NULL TX buffer pointer"), so powerctl's
    // pcaWriteReg() fails, epd_painter_powerctl::begin() returns false, and the
    // library does:
    //     printf("FATAL: powerctl init failed!\n");  while (1) delay(1000);
    // setup() never returns, no tasks are created, the panel stays blank and
    // serial stays silent while USB remains enumerated. The FATAL line goes to
    // newlib stdout -> UART0 -> GPIO43, which is BOARD_GPS_TXD, so nothing on
    // the USB console ever sees it either.
    //
    // tools/epdpainter_test works only because it never calls Wire.begin(), so
    // the library's own TwoWire is the first to init the bus and does allocate.
    //
    // Sharing the one instance also puts the driver's power-control traffic
    // (PanelPowerGuard runs from the epd_paint task) behind the same TwoWire
    // lock as our touch/fuel-gauge/RTC reads, instead of a second, independent
    // lock guarding the same hardware.
    EPD_Painter::Config cfg = EPD_LILYGO_T5_S3_GPS_PRESET;
    cfg.i2c.wire = &Wire;

    // Landscape here: our UI does its own rotation through epd_set_rotation()
    // below, and letting the driver rotate as well would apply it twice.
    static EPD_Painter painter(cfg, /*portrait=*/false);

    // MUST be off, and it defaults to on.
    //
    // EPD_Painter ships a power-button emulation: EPD_BootCtl treats "reset was
    // pressed while running" as a shutdown request, and at the end of begin() it
    // paints a boot image and calls _powerOff(), which is [[noreturn]]. For a
    // standalone e-paper app with no other power management that is a feature. For
    // us it is fatal — begin() never returns, so setup() never finishes, no tasks
    // are ever created, and the device sits there blank and silent on serial while
    // USB stays enumerated. That is exactly the symptom this cost a long debugging
    // session to explain.
    //
    // Its one escape hatch is _isUsbConnected(), which probes a BQ25896 charger at
    // I2C 0x6B. That read does not succeed here — different charger, and the
    // library probes on its own TwoWire instance — so "no USB" is the verdict on
    // every boot, USB cable or not.
    //
    // We already own shutdown: shutdownDevice() draws the farewell screen and
    // calls esp_deep_sleep_start(). Reset must mean reset.
    painter.setAutoShutdown(false);

    // Push the 129,600-byte fastbuffer into PSRAM so NimBLE can have the
    // internal RAM back.
    //
    // Internal DRAM is oversubscribed on this build: ~137 KB static + ~160 KB
    // for EPD_Painter + ~60 KB for the BLE controller does not fit in 320 KB.
    // Measured, with the display initialised first: 209 KB internal free before
    // begin(), 49 KB after — and NimBLEDevice::init() then hangs with a 34 KB
    // largest block. Initialising BLE first instead just moves the failure: the
    // display is then ~1 KB short on dec_sweeps and begin() returns false
    // silently at EPD_Painter.cpp:780, which is the blank screen this port
    // started with. Neither order fits.
    //
    // Nearly all of the display's internal usage is one allocation, and the
    // library already knows how to do without it:
    //     packed_fastbuffer = heap_caps_aligned_alloc(16, packed_size, INTERNAL);
    //     if (!packed_fastbuffer) { log_w(...); ... alloc in SPIRAM; }
    // There is no config flag to choose, so make an internal block that size
    // simply unavailable while begin() runs. The decision tables (dec_sweeps is
    // the largest at ~13 KB) still come from internal, which is what they need.
    //
    // Costs paint throughput — the fastbuffer is read on every scan line, and
    // the library's own comment calls the PSRAM path "slower for the per-pixel
    // ops in epd_painter_ink_dual() but boots cleanly". Worth measuring; if it
    // proves too slow the real fix is a library patch taking the placement as a
    // Config field, rather than trimming elsewhere to win back 129 KB.
    const size_t fastBytes = (size_t)kNativeW * kNativeH / 4;   // 129600
    const size_t keepFree = 48 * 1024;    // dec_* + DMA rows + paint-task stack
    void* ballast = nullptr;
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (largest > fastBytes) {
        ballast = heap_caps_malloc(largest - keepFree, MALLOC_CAP_INTERNAL);
        Serial.printf("[epdc] fastbuffer->PSRAM: reserved %u internal, "
                      "largest now %u (need >%u to stay internal)\n",
                      (unsigned)(largest - keepFree),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)fastBytes);
    }

    const bool painterOk = painter.begin();
    if (ballast) heap_caps_free(ballast);   // hand it straight back to NimBLE

    if (!painterOk) {
        Serial.printf("[epdc] EPD_Painter::begin() failed; "
                      "internal=%u (largest %u) psram=%u\n",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return false;
    }
    g_painter = &painter;

    epd_set_rotation(DISPLAY_ROTATION);
    Serial.printf("[epdc] ready: %d grey levels, internal left=%u psram left=%u\n",
                  g_painter->greyLevels(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}

uint8_t* epdc_framebuffer() { return g_fb4; }

int epdc_grey_levels() { return g_painter ? g_painter->greyLevels() : 0; }

// Expand 4bpp -> 8bpp levels. Two output pixels per input byte, so this is
// one pass over 259200 bytes. Shared by paint and the dirty-rect scrub, which
// needs the same 8bpp frame to diff the glass against.
static void expandLevels() {
    const uint8_t* src = g_fb4;
    uint8_t* dst = g_levels;
    for (int y = 0; y < kNativeH; ++y) {
        for (int b = 0; b < kBytesPerRow; ++b) {
            const uint8_t byte = *src++;
            *dst++ = g_lut[byte & 0x0F];   // even x
            *dst++ = g_lut[byte >> 4];     // odd x
        }
    }
}

// -DEPDC_PAINT_TRACE splits epdc_paint() into its two halves on the console.
// The boot loop under investigation resets INSIDE this function, and the two
// halves fail in completely different places: expandLevels() is our own PSRAM
// loop, paint() is the driver clocking the panel. Without this the trace can
// only say "somewhere in epdc_paint".
#ifdef EPDC_PAINT_TRACE
#define PAINT_TRACE(msg) Serial.printf("[epdc] %lu ms: %s\n", \
                                       (unsigned long)millis(), msg)
#else
#define PAINT_TRACE(msg) ((void)0)
#endif

void epdc_paint() {
    if (!g_painter) return;
    PAINT_TRACE("expand enter");
    expandLevels();
    PAINT_TRACE("expand done, paint enter");
    g_painter->paint(g_levels);
    PAINT_TRACE("paint returned");
}

void epdc_clear_dirty(int tolerance) {
    if (!g_painter) return;
    // clearDirtyAreas() compacts this frame into the driver's paintbuffer to
    // diff it against the screenbuffer, then whitens the rectangles that differ.
    // It does NOT paint the new content — paint() recompacts the same frame, so
    // the caller's usual epdc_paint() still follows.
    expandLevels();
    g_painter->clearDirtyAreas(g_levels, tolerance, EPD_Painter::ClearMode::SOFT);
}

void epdc_paint_wait() {
    if (!g_painter) return;
    // paintIdle() is paintStage == 0, i.e. the paint task has finished driving
    // and gone back to waiting. Bounded so a wedged driver can never turn a
    // power-off into a hang — a truncated farewell screen beats a device that
    // will not switch off. 6 s is far beyond a full-panel paint (~1.9 s of the
    // 4x clear at boot is four of them).
    const uint32_t deadline = millis() + 6000;
    while (!g_painter->paintIdle() && (int32_t)(millis() - deadline) < 0) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (!g_painter->paintIdle())
        Serial.println("[epdc] paint did not finish within 6 s; powering off anyway");
}

// The driver's idle timer re-arms to `_idle_timeout_s` on every paint, so
// shortening it here means the farewell paint that follows arms one second rather
// than five.
void epdc_set_idle_timeout(int seconds) {
    if (g_painter) g_painter->setIdleTimeout(seconds);
}

void epdc_power_off_soon() {
    if (g_painter) g_painter->setIdleTimeout(1);
}

void epdc_power_off_wait() {
    if (!g_painter) return;
    // A blind wait, deliberately: the driver exposes no rail-state getter, so
    // there is nothing to poll. Its `panel_idle_off` task ticks at 1 Hz and powers
    // off when the count armed above reaches zero, which is two ticks away worst
    // case. That task runs at priority 1 and this is the UI task at 2, so the
    // delay is what lets it be scheduled at all.
    vTaskDelay(pdMS_TO_TICKS(2400));
}

void epdc_clear(int passes) {
    if (!g_painter) return;
    if (passes < 1) passes = 1;
    for (int i = 0; i < passes; ++i) {
#ifdef EPDC_BOOT_WAIT
        // BRING-UP TRACE. EPD_Painter::clear() contains two unbounded waits: a
        // `while (paintStage == 1) vTaskDelay(1)` spin that only the epd_paint
        // task can break, and an xSemaphoreTake(_paint_active_sem,
        // portMAX_DELAY). Either hangs setup() forever with no output, so log
        // per pass — "clear 0/4 enter" with no "done" pins the hang on the very
        // first pass (paint task never scheduled) rather than on a later one.
        // Gated: this fires on every clear, boot and shutdown alike, which is
        // noise once the panel is known to come up.
        Serial.printf("[epdc] clear %d/%d enter (%lu ms)\n", i, passes,
                      (unsigned long)millis());
#endif
        g_painter->clear();
#ifdef EPDC_BOOT_WAIT
        Serial.printf("[epdc] clear %d/%d done (%lu ms)\n", i, passes,
                      (unsigned long)millis());
#endif
    }
}

#endif  // ARDUINO

#endif  // !ARDUINO || USE_EPD_PAINTER
