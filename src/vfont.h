#pragma once
// Vector fonts, rasterised on the device at whatever size a cell wants.
//
// Why: the bitmap ladders (9 Impact faces, 4 Arial faces, 7 MB of headers)
// pin every value to the nearest pre-rendered height, and adding a weight or
// a size means another megabyte of flash. A vector face is ~15 KB of
// quadratic outlines per weight, and it renders at the EXACT height that fits.
//
// How it plugs in: vf_face() hands back an ordinary `const EpdFont*`. Its
// glyphs are rasterised lazily, on first use, into the same 4-bit epdiy glyph
// layout the bitmap fonts use, so text()/textWidth()/epdc_digit_height() and
// the ladders keep working untouched — epd_compat's findGlyph() asks
// vf_glyph() first. Faces are cached (font, size) -> rendered glyphs in PSRAM
// and evicted LRU when the pool is full.
//
// Outline data comes from tools/vfont/vfont_build.py (TTF -> vf_*.h).
#include <stdint.h>
#include <stddef.h>
#include "epd_compat.h"

struct VfGlyph {
    uint16_t adv;                     // advance width, font units
    int16_t xmin, ymin, xmax, ymax;   // outline bbox, font units (y up)
    uint32_t pointOffset;             // index into VfFont::points (pairs)
    uint16_t pointCount;
};

struct VfFont {
    uint16_t unitsPerEm;
    int16_t ascender, descender;      // hhea, font units (descender negative)
    int16_t capHeight;
    int16_t digitTop, digitBottom;    // '0' ink box: size faces by digit height
    uint32_t glyphCount;
    const VfGlyph* glyphs;
    const int16_t* points;            // x=(X<<1)|onCurve, y=(Y<<1)|contourEnd
    const EpdUnicodeInterval* intervals;
    uint32_t intervalCount;
};

// A face is a font at a scale. `emPx` is the em size in pixels; use
// vf_face_digit() to size by the ink height of '0', which is what the value
// ladder reasons in (epdc_digit_height). Both return a cached EpdFont whose
// glyphs render on demand. Sizes are quantised to 1/4 px so near-identical
// requests share a face. Never returns null for a non-null font.
const EpdFont* vf_face(const VfFont* font, float emPx);
const EpdFont* vf_face_digit(const VfFont* font, int digitPx);
// Size by cap height — how the label faces compare (Arial_B's caps are 14 px).
const EpdFont* vf_face_cap(const VfFont* font, float capPx);

// Largest digit height (px) whose rendering of `str` fits availW x availH —
// the vector replacement for valueFontIndex(). Binary-searches faces, so it
// touches at most ~8 cache entries. Returns 0 if not even minPx fits.
int vf_fit_digit(const VfFont* font, const char* str, int availW, int availH,
                 int unitW, int minPx = 12, int maxPx = 200);

// Hook for epd_compat::findGlyph. Non-null iff `font` is a vf face.
const EpdGlyph* vf_glyph(const EpdFont* font, uint32_t cp);

// Drop every cached face (tests; or when the arena must be reclaimed).
void vf_flush();
// Bytes currently held by rendered glyph bitmaps, all faces.
size_t vf_cache_bytes();
