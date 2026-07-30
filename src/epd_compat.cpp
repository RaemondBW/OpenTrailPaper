// epdiy-compatible software rasteriser. See epd_compat.h for why this exists.
//
// Written from scratch (standard Bresenham / scanline algorithms and a 4bpp
// glyph blit) rather than adapted from epdiy, which is LGPL-3.0 while this
// project is Apache-2.0.

#include "epd_compat.h"

#include <stdlib.h>
#include <string.h>

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

void epd_draw_line(int x0, int y0, int x1, int y1, uint8_t color, uint8_t* fb) {
    const int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        epd_draw_pixel(x0, y0, color, fb);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
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

// Filled circle as horizontal spans, so it is solid with no seams.
void epd_fill_circle(int x0, int y0, int r, uint8_t color, uint8_t* fb) {
    for (int dy = -r; dy <= r; ++dy) {
        // Largest dx with dx^2 + dy^2 <= r^2, without floating point.
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;
        epd_draw_hline(x0 - dx, y0 + dy, 2 * dx + 1, color, fb);
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

int stringWidth(const EpdFont* font, const char* s) {
    int w = 0;
    while (*s) {
        const uint32_t cp = nextCodePoint(&s);
        const EpdGlyph* g = findGlyph(font, cp);
        if (g) w += g->advance_x;
    }
    return w;
}

// Blit one glyph. The bitmap is 4bpp, 2 pixels per byte, low nibble first, each
// nibble an intensity 0..15 (0 = leave background alone).
//
// We THRESHOLD that intensity to fully on/off rather than blending. The panel is
// driven 1-bit for text, and the anti-aliased edge values were a real problem
// with epdiy: a pixel stuck between black and white gives the waveform no
// stable target and contributed to the grey drift (see
// investigations/display-ghosting.md). Thresholding also makes text render
// identically on the device and in the host preview.
void blitGlyph(const EpdFont* font, const EpdGlyph* g, int* cursorX, int cursorY,
               uint8_t fgLevel, uint8_t* fb) {
    const int byteWidth = (g->width + 1) / 2;
    const uint8_t* bmp = &font->bitmap[g->data_offset];
    const uint8_t color = (uint8_t)(fgLevel << 4);

    for (int y = 0; y < g->height; ++y) {
        const int py = cursorY - g->top + y;
        for (int x = 0; x < g->width; ++x) {
            const uint8_t byte = bmp[y * byteWidth + (x >> 1)];
            const uint8_t v = (x & 1) ? (byte >> 4) : (byte & 0x0F);
            if (v >= 8) epd_draw_pixel(*cursorX + g->left + x, py, color, fb);
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
    int x = *cursor_x;
    const EpdFontFlags align = (EpdFontFlags)(props.flags & 0x3);
    if (align == EPD_DRAW_ALIGN_CENTER)     x -= stringWidth(font, string) / 2;
    else if (align == EPD_DRAW_ALIGN_RIGHT) x -= stringWidth(font, string);

    const char* s = string;
    while (*s) {
        const uint32_t cp = nextCodePoint(&s);
        const EpdGlyph* g = findGlyph(font, cp);
        if (!g) g = findGlyph(font, props.fallback_glyph);
        if (!g) continue;
        blitGlyph(font, g, &x, *cursor_y, props.fg_color, fb);
    }
    *cursor_x = x;
    return EPD_DRAW_SUCCESS;
}

EpdRect epd_get_string_rect(const EpdFont* font, const char* string, int x, int y,
                            int margin, const EpdFontProperties* properties) {
    (void)properties;
    EpdRect r = {0, 0, 0, 0};
    if (!font || !string) return r;
    const int w = stringWidth(font, string);
    const int h = font->ascender - font->descender;
    r.x = (uint16_t)(x - margin);
    r.y = (uint16_t)(y - font->ascender - margin);
    r.width = (uint16_t)(w + 2 * margin);
    r.height = (uint16_t)(h + 2 * margin);
    return r;
}

// ---------------------------------------------------------------------------
// Panel layer — EPD_Painter
// ---------------------------------------------------------------------------
//
// Only compiled for the device. The host preview harness links the rasteriser
// above and writes PNGs itself, so it must not pull in EPD_Painter or ESP-IDF.
#ifdef ARDUINO

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

    g_fb4 = (uint8_t*)heap_caps_malloc(fb4Size, MALLOC_CAP_SPIRAM);
    g_levels = (uint8_t*)heap_caps_aligned_alloc(16, lvlSize, MALLOC_CAP_SPIRAM);
    if (!g_fb4 || !g_levels) return false;
    memset(g_fb4, 0xFF, fb4Size);      // white
    memset(g_levels, 0, lvlSize);      // level 0 = white

    buildLut();

    // Landscape here: our UI does its own rotation through epd_set_rotation()
    // below, and letting the driver rotate as well would apply it twice.
    static EPD_Painter painter(EPD_LILYGO_T5_S3_GPS_PRESET, /*portrait=*/false);
    if (!painter.begin()) return false;
    g_painter = &painter;

    epd_set_rotation(DISPLAY_ROTATION);
    return true;
}

uint8_t* epdc_framebuffer() { return g_fb4; }

int epdc_grey_levels() { return g_painter ? g_painter->greyLevels() : 0; }

void epdc_paint() {
    if (!g_painter) return;
    // Expand 4bpp -> 8bpp levels. Two output pixels per input byte, so this is
    // one pass over 259200 bytes.
    const uint8_t* src = g_fb4;
    uint8_t* dst = g_levels;
    for (int y = 0; y < kNativeH; ++y) {
        for (int b = 0; b < kBytesPerRow; ++b) {
            const uint8_t byte = *src++;
            *dst++ = g_lut[byte & 0x0F];   // even x
            *dst++ = g_lut[byte >> 4];     // odd x
        }
    }
    g_painter->paint(g_levels);
}

void epdc_clear(int passes) {
    if (!g_painter) return;
    if (passes < 1) passes = 1;
    for (int i = 0; i < passes; ++i) g_painter->clear();
}

#endif  // ARDUINO
