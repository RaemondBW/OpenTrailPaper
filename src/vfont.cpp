// Vector font rasteriser + face cache. See vfont.h for the why.
//
// The rasteriser is the signed-area accumulation scheme from font-rs
// (Raph Levien, Apache-2/MIT): every edge deposits its signed coverage
// delta into a float buffer, and a prefix sum along each row turns that into
// exact-area antialiased coverage. It is a few dozen lines, has no
// per-scanline edge list, and costs O(edge length + area) — a 160 px digit
// renders in well under a millisecond on the S3's FPU. Non-zero winding is
// approximated as |accumulated| clamped to 1, which is exact for the
// non-self-overlapping contours real fonts have.
#include "vfont.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
static void* vfAlloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
static void* vfRealloc(void* p, size_t n) { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
#else
static void* vfAlloc(size_t n) { return malloc(n); }
static void* vfRealloc(void* p, size_t n) { return realloc(p, n); }
#endif
static void vfFree(void* p) { free(p); }

namespace {

// --- accumulation rasteriser -----------------------------------------------

struct Raster {
    int w, h;
    float* a;   // (w * h + 4) cells

    void line(float x0, float y0, float x1, float y1) {
        if (fabsf(y0 - y1) <= 1e-6f) return;
        float dir = 1.f;
        if (y0 > y1) { dir = -1.f; float t; t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
        const float dxdy = (x1 - x0) / (y1 - y0);
        float x = x0;
        int yi = y0 < 0 ? 0 : (int)y0;
        if (y0 < 0) x -= y0 * dxdy;
        const int yend = (int)ceilf(y1) < h ? (int)ceilf(y1) : h;
        for (; yi < yend; ++yi) {
            const int ls = yi * w;
            const float yb = (float)yi > y0 ? (float)yi : y0;
            const float yt = (float)(yi + 1) < y1 ? (float)(yi + 1) : y1;
            const float dy = yt - yb;
            const float xnext = x + dxdy * dy;
            const float d = dy * dir;
            float xa = x, xb = xnext;
            if (xa > xb) { float t = xa; xa = xb; xb = t; }
            const float xaf = floorf(xa);
            const int xai = (int)xaf;
            const float xbc = ceilf(xb);
            const int xbi = (int)xbc;
            if (xai < 0 || xbi >= w) { x = xnext; continue; }   // clipped: bbox pad makes this rare
            if (xbi <= xai + 1) {
                const float xmf = 0.5f * (x + xnext) - xaf;
                a[ls + xai] += d - d * xmf;
                a[ls + xai + 1] += d * xmf;
            } else {
                const float s = 1.f / (xb - xa);
                const float x0f = xa - xaf;
                const float a0 = 0.5f * s * (1.f - x0f) * (1.f - x0f);
                const float x1f = xb - xbc + 1.f;
                const float am = 0.5f * s * x1f * x1f;
                a[ls + xai] += d * a0;
                if (xbi == xai + 2) {
                    a[ls + xai + 1] += d * (1.f - a0 - am);
                } else {
                    const float a1 = s * (1.5f - x0f);
                    a[ls + xai + 1] += d * (a1 - a0);
                    for (int xi = xai + 2; xi < xbi - 1; ++xi) a[ls + xi] += d * s;
                    const float a2 = a1 + (float)(xbi - xai - 3) * s;
                    a[ls + xbi - 1] += d * (1.f - a2 - am);
                }
                a[ls + xbi] += d * am;
            }
            x = xnext;
        }
    }

    void quad(float x0, float y0, float cx, float cy, float x1, float y1) {
        const float devx = x0 - 2.f * cx + x1, devy = y0 - 2.f * cy + y1;
        const float devsq = devx * devx + devy * devy;
        if (devsq < 0.333f) { line(x0, y0, x1, y1); return; }
        const int n = 1 + (int)floorf(sqrtf(sqrtf(3.f * devsq)));
        float px = x0, py = y0;
        for (int i = 1; i < n; ++i) {
            const float t = (float)i / (float)n, u = 1.f - t;
            const float qx = u * u * x0 + 2.f * u * t * cx + t * t * x1;
            const float qy = u * u * y0 + 2.f * u * t * cy + t * t * y1;
            line(px, py, qx, qy);
            px = qx; py = qy;
        }
        line(px, py, x1, y1);
    }
};

// Walk one glyph's contours, emitting quads/lines into `r`. Points are in
// font units; `sx/sy` map them to raster space (y flipped, origin shifted).
struct Xf { float s, ox, oy; float x(int X) const { return X * s + ox; } float y(int Y) const { return oy - Y * s; } };

void traceGlyph(const VfFont* f, const VfGlyph* g, const Xf& xf, Raster& r) {
    const int16_t* p = f->points + 2 * g->pointOffset;
    const int n = g->pointCount;
    int start = 0;
    while (start < n) {
        int end = start;
        while (!(p[2 * end + 1] & 1)) ++end;   // contourEnd flag
        const int cnt = end - start + 1;
        // Decoded contour accessor.
        auto X = [&](int i) { return (int)(p[2 * (start + (i % cnt))] >> 1); };
        auto Y = [&](int i) { return (int)(p[2 * (start + (i % cnt)) + 1] >> 1); };
        auto on = [&](int i) { return (p[2 * (start + (i % cnt))] & 1) != 0; };
        // Find a starting on-curve point (or synthesise one between two offs).
        float sx, sy; int i0 = 0;
        if (on(0)) { sx = xf.x(X(0)); sy = xf.y(Y(0)); i0 = 0; }
        else if (on(cnt - 1)) { sx = xf.x(X(cnt - 1)); sy = xf.y(Y(cnt - 1)); i0 = cnt - 1; }
        else { sx = 0.5f * (xf.x(X(0)) + xf.x(X(cnt - 1))); sy = 0.5f * (xf.y(Y(0)) + xf.y(Y(cnt - 1))); i0 = cnt - 1; }
        float cx = sx, cy = sy;          // current on-curve position
        bool haveCtrl = false; float kx = 0, ky = 0;
        for (int k = 1; k <= cnt; ++k) {
            const int i = i0 + k;
            const float px = xf.x(X(i)), py = xf.y(Y(i));
            if (k == cnt) {
                // Back at the start point.
                if (haveCtrl) r.quad(cx, cy, kx, ky, sx, sy); else r.line(cx, cy, sx, sy);
                break;
            }
            if (on(i)) {
                if (haveCtrl) { r.quad(cx, cy, kx, ky, px, py); haveCtrl = false; }
                else r.line(cx, cy, px, py);
                cx = px; cy = py;
            } else if (haveCtrl) {
                const float mx = 0.5f * (kx + px), my = 0.5f * (ky + py);
                r.quad(cx, cy, kx, ky, mx, my);
                cx = mx; cy = my; kx = px; ky = py;
            } else { kx = px; ky = py; haveCtrl = true; }
        }
        start = end + 1;
    }
}

// --- face cache --------------------------------------------------------------

constexpr int FACE_POOL = 16;
constexpr float SIZE_Q = 4.f;   // quantise em size to 1/4 px

struct Face {
    const VfFont* font = nullptr;
    float scale = 0;             // px per font unit
    EpdFont epd{};
    EpdGlyph* glyphs = nullptr;
    uint8_t* done = nullptr;     // one byte per glyph: rendered?
    uint8_t* arena = nullptr;
    size_t used = 0, cap = 0;
    uint32_t lastUse = 0;
};

Face g_faces[FACE_POOL];
uint32_t g_clock = 0;

void freeFace(Face& f) {
    vfFree(f.glyphs); vfFree(f.done); vfFree(f.arena);
    f = Face{};
}

const VfGlyph* findVf(const VfFont* f, uint32_t cp, uint32_t* index) {
    for (uint32_t i = 0; i < f->intervalCount; ++i) {
        const EpdUnicodeInterval& iv = f->intervals[i];
        if (cp >= iv.first && cp <= iv.last) {
            *index = iv.offset + (cp - iv.first);
            return &f->glyphs[*index];
        }
    }
    return nullptr;
}

// Rasterise glyph `gi` of `face` into its arena and fill the EpdGlyph.
bool renderGlyph(Face& face, uint32_t gi) {
    const VfFont* f = face.font;
    const VfGlyph* g = &f->glyphs[gi];
    EpdGlyph& out = face.glyphs[gi];
    const float s = face.scale;
    out = EpdGlyph{};
    out.advance_x = (uint16_t)lroundf(g->adv * s);
    if (g->pointCount == 0) { face.done[gi] = 1; return true; }

    // Padded raster box (1 px each side) so edges never clip.
    const int x0 = (int)floorf(g->xmin * s) - 1, x1 = (int)ceilf(g->xmax * s) + 1;
    const int yt = (int)ceilf(g->ymax * s) + 1, yb = (int)floorf(g->ymin * s) - 1;
    const int w = x1 - x0, h = yt - yb;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) return false;

    float* acc = (float*)vfAlloc(sizeof(float) * (size_t)(w * h + 4));
    if (!acc) return false;
    memset(acc, 0, sizeof(float) * (size_t)(w * h + 4));
    Raster r{w, h, acc};
    Xf xf{s, -(float)x0, (float)yt};
    traceGlyph(f, g, xf, r);

    // Prefix-sum to coverage (4-bit), tracking the ink box to crop to.
    uint8_t* cov = (uint8_t*)vfAlloc((size_t)w * h);
    if (!cov) { vfFree(acc); return false; }
    int ix0 = w, ix1 = -1, iy0 = h, iy1 = -1;
    for (int y = 0; y < h; ++y) {
        float a = 0;
        for (int x = 0; x < w; ++x) {
            a += acc[y * w + x];
            float c = fabsf(a); if (c > 1.f) c = 1.f;
            const uint8_t v = (uint8_t)(c * 15.f + 0.5f);
            cov[y * w + x] = v;
            if (v) { if (x < ix0) ix0 = x; if (x > ix1) ix1 = x; if (y < iy0) iy0 = y; if (y > iy1) iy1 = y; }
        }
    }
    vfFree(acc);
    if (ix1 < 0) { vfFree(cov); face.done[gi] = 1; return true; }   // blank

    const int gw = ix1 - ix0 + 1, gh = iy1 - iy0 + 1;
    const size_t bytes = (size_t)((gw + 1) / 2) * gh;
    if (face.used + bytes > face.cap) {
        size_t ncap = face.cap ? face.cap * 2 : 8192;
        while (ncap < face.used + bytes) ncap *= 2;
        uint8_t* na = (uint8_t*)vfRealloc(face.arena, ncap);
        if (!na) { vfFree(cov); return false; }
        face.arena = na; face.cap = ncap;
        face.epd.bitmap = face.arena;
    }
    uint8_t* dst = face.arena + face.used;
    memset(dst, 0, bytes);
    const int bw = (gw + 1) / 2;
    for (int y = 0; y < gh; ++y)
        for (int x = 0; x < gw; ++x) {
            const uint8_t v = cov[(iy0 + y) * w + ix0 + x];
            dst[y * bw + (x >> 1)] |= (x & 1) ? (uint8_t)(v << 4) : v;
        }
    vfFree(cov);

    out.width = (uint16_t)gw;
    out.height = (uint16_t)gh;
    out.left = (int16_t)(x0 + ix0);
    out.top = (int16_t)(yt - iy0);
    out.data_offset = (uint32_t)face.used;
    face.used += bytes;
    face.done[gi] = 1;
    return true;
}

Face* faceOf(const EpdFont* font) {
    for (int i = 0; i < FACE_POOL; ++i)
        if (g_faces[i].font && &g_faces[i].epd == font) return &g_faces[i];
    return nullptr;
}

Face* acquire(const VfFont* font, float scale) {
    ++g_clock;
    Face* victim = nullptr;
    for (int i = 0; i < FACE_POOL; ++i) {
        Face& f = g_faces[i];
        if (f.font == font && f.scale == scale) { f.lastUse = g_clock; return &f; }
        if (!victim || !f.font || (victim->font && f.lastUse < victim->lastUse)) {
            if (!victim || !f.font || victim->font) victim = &f;
        }
    }
    freeFace(*victim);
    Face& f = *victim;
    f.font = font;
    f.scale = scale;
    f.lastUse = g_clock;
    f.glyphs = (EpdGlyph*)vfAlloc(sizeof(EpdGlyph) * font->glyphCount);
    f.done = (uint8_t*)vfAlloc(font->glyphCount);
    if (!f.glyphs || !f.done) { freeFace(f); return nullptr; }
    memset(f.glyphs, 0, sizeof(EpdGlyph) * font->glyphCount);
    memset(f.done, 0, font->glyphCount);
    f.epd.bitmap = nullptr;
    f.epd.glyph = f.glyphs;
    f.epd.intervals = font->intervals;
    f.epd.interval_count = font->intervalCount;
    f.epd.compressed = false;
    f.epd.ascender = (int)lroundf(font->ascender * scale);
    f.epd.descender = (int)lroundf(font->descender * scale);
    f.epd.advance_y = (uint16_t)(f.epd.ascender - f.epd.descender);
    return &f;
}

}  // namespace

const EpdFont* vf_face(const VfFont* font, float emPx) {
    if (!font) return nullptr;
    const float q = roundf(emPx * SIZE_Q) / SIZE_Q;
    Face* f = acquire(font, q / (float)font->unitsPerEm);
    return f ? &f->epd : nullptr;
}

const EpdFont* vf_face_digit(const VfFont* font, int digitPx) {
    if (!font) return nullptr;
    const int dh = font->digitTop - font->digitBottom;
    if (dh <= 0) return vf_face(font, (float)digitPx);
    return vf_face(font, (float)digitPx * (float)font->unitsPerEm / (float)dh);
}

const EpdFont* vf_face_cap(const VfFont* font, float capPx) {
    if (!font) return nullptr;
    if (font->capHeight <= 0) return vf_face(font, capPx * 1.4f);
    return vf_face(font, capPx * (float)font->unitsPerEm / (float)font->capHeight);
}

const EpdGlyph* vf_glyph(const EpdFont* font, uint32_t cp) {
    Face* f = faceOf(font);
    if (!f) return nullptr;
    uint32_t gi;
    if (!findVf(f->font, cp, &gi)) return nullptr;
    if (!f->done[gi] && !renderGlyph(*f, gi)) return nullptr;
    f->lastUse = ++g_clock;
    return &f->glyphs[gi];
}

int vf_fit_digit(const VfFont* font, const char* str, int availW, int availH,
                 int unitW, int minPx, int maxPx) {
    auto fits = [&](int px) {
        const EpdFont* f = vf_face_digit(font, px);
        if (!f) return false;
        if (epdc_digit_height(f) > availH) return false;
        EpdFontProperties p = epd_font_properties_default();
        EpdRect r = epd_get_string_rect(f, str, 0, 0, 0, &p);
        return r.width + unitW <= availW;
    };
    if (!fits(minPx)) return 0;
    int lo = minPx, hi = maxPx;
    if (fits(hi)) return hi;
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        if (fits(mid)) lo = mid; else hi = mid;
    }
    return lo;
}

void vf_flush() {
    for (int i = 0; i < FACE_POOL; ++i) freeFace(g_faces[i]);
}

size_t vf_cache_bytes() {
    size_t n = 0;
    for (int i = 0; i < FACE_POOL; ++i) n += g_faces[i].used;
    return n;
}
