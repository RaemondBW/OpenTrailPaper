#include "test_clip.h"

#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

namespace test_clip {

namespace {

constexpr int kW = 480, kH = 270;
constexpr int kStride = kW / 8;
constexpr size_t kFrameBytes = (size_t)kStride * kH;

constexpr Section kList[kSections] = {
    {"ball", "small motion over a static dither: nothing else may be disturbed", 0, 45},
    {"text", "thin lines scrolling: fine detail, opposite polarity side by side", 45, 45},
    {"pan", "map-like pan: most of the panel moves, heavy dark areas", 90, 45},
    {"plasma", "continuous tone in motion: what real video does to the panel", 135, 45},
    {"strobe", "patches inverting faster than a drive finishes: cancellation", 180, 30},
    {"cuts", "full-panel inversions every frame: worst case for every buffer", 210, 20},
};
constexpr int kFrames = 230;

uint8_t* g_clip = nullptr;
uint8_t* g_card = nullptr;
uint32_t g_expand[256];
int8_t g_sin[256];
uint8_t g_row[kStride];

// Ordered, not error-diffused: a pixel's threshold never moves, so a still area
// stays still and only real change reaches the panel.
const uint8_t kBayer[8][8] = {
    {0, 32, 8, 40, 2, 34, 10, 42},  {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44, 4, 36, 14, 46, 6, 38}, {60, 28, 52, 20, 62, 30, 54, 22},
    {3, 35, 11, 43, 1, 33, 9, 41},  {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47, 7, 39, 13, 45, 5, 37}, {63, 31, 55, 23, 61, 29, 53, 21},
};

inline void rowClear() { memset(g_row, 0, sizeof g_row); }
inline void put(int x) { g_row[x >> 3] |= (uint8_t)(0x80 >> (x & 7)); }
inline void span(int x0, int x1) {
    if (x0 < 0) x0 = 0;
    if (x1 > kW) x1 = kW;
    for (int x = x0; x < x1; ++x) put(x);
}
// tone: 0 white .. 64 black
inline void shade(int x, int y, int tone) {
    if (tone > kBayer[y & 7][x & 7]) put(x);
}

void ball(int i, int y) {
    rowClear();
    for (int x = 0; x < kW; ++x) shade(x, y, (x * 56) / kW + 4);
    // Two discs on different paths, so two overlapping updates are always live.
    const int cx[2] = {60 + (i * 8) % 360, 420 - (i * 5) % 360};
    const int cy[2] = {135 + (g_sin[(i * 7) & 255] * 90) / 127,
                       135 + (g_sin[(i * 5 + 64) & 255] * 100) / 127};
    for (int b = 0; b < 2; ++b) {
        const int dy = y - cy[b];
        if (dy * dy >= 28 * 28) continue;
        const int half = (int)sqrtf((float)(28 * 28 - dy * dy));
        for (int x = cx[b] - half; x < cx[b] + half; ++x) {
            if (x < 0 || x >= kW) continue;
            if (b) g_row[x >> 3] &= (uint8_t)~(0x80 >> (x & 7));
            else put(x);
        }
    }
}

void text(int i, int y) {
    rowClear();
    const int v = y + i * 6;
    const int line = v / 12;
    if (v % 12 >= 7) return;
    // Word-shaped runs whose positions depend on the line, one-pixel stems inside.
    uint32_t seed = (uint32_t)line * 2654435761u;
    for (int x = 12; x < kW - 12;) {
        seed = seed * 1664525u + 1013904223u;
        const int len = 12 + (seed >> 28) * 5;
        for (int k = x; k < x + len && k < kW - 12; k += 2) put(k);
        x += len + 8;
    }
}

void pan(int i, int y) {
    rowClear();
    const int shift = i * 7;
    const int coast = 210 + (g_sin[((y + shift) / 2) & 255] * 90) / 127;
    for (int x = 0; x < coast; ++x) shade(x, y, 48);
    for (int r = 0; r < 4; ++r) {
        const int ry = (r * 85 + shift) % (kH + 30) - 15;
        if (y >= ry && y < ry + 3) span(0, kW);
    }
    const int rx = (shift * 2) % (kW + 40) - 20;
    span(rx, rx + 3);
}

void plasma(int i, int y) {
    rowClear();
    for (int x = 0; x < kW; ++x) {
        const int v = g_sin[(x + i * 5) & 255] + g_sin[(y * 2 - i * 3) & 255] +
                      g_sin[((x + y) / 2 + i * 4) & 255];
        shade(x, y, 32 + (v * 32) / 381);
    }
}

// Three patches inverting every frame, every second frame and every fourth, on a
// background that must not move.
void strobe(int i, int y) {
    rowClear();
    if (y < 40 || y >= 230) return;
    if ((i & 1) == 0) span(40, 150);
    if ((i & 2) == 0) span(185, 295);
    if ((i & 4) == 0) span(330, 440);
}

void cuts(int i, int y) {
    switch (i & 3) {
        case 0: memset(g_row, 0xFF, sizeof g_row); break;
        case 1: memset(g_row, 0x00, sizeof g_row); break;
        case 2: memset(g_row, (y & 8) ? 0xF0 : 0x0F, sizeof g_row); break;
        default: memset(g_row, (y & 8) ? 0x0F : 0xF0, sizeof g_row); break;
    }
}

void expandFrom(const uint8_t* src, uint8_t* packed) {
    uint32_t row[kStride];
    for (int y = 0; y < kH; ++y) {
        for (int b = 0; b < kStride; ++b) row[b] = g_expand[src[b]];
        memcpy(packed + (size_t)y * 2 * 240, row, 240);
        memcpy(packed + ((size_t)y * 2 + 1) * 240, row, 240);
        src += kStride;
    }
}

}  // namespace

const Section kSection[kSections] = {kList[0], kList[1], kList[2], kList[3], kList[4], kList[5]};

int frameCount() { return kFrames; }

bool render() {
    if (g_clip) return true;
    uint8_t* clip = (uint8_t*)heap_caps_malloc(kFrameBytes * kFrames, MALLOC_CAP_SPIRAM);
    uint8_t* card = (uint8_t*)heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM);
    if (!clip || !card) return false;

    for (int i = 0; i < 256; ++i) g_sin[i] = (int8_t)(127 * sinf(i * 2 * (float)M_PI / 256));
    // One source pixel becomes two 2bpp panel pixels, so a source byte becomes four
    // panel bytes, first pixel in the MSBs of the first. Words are little-endian.
    for (int v = 0; v < 256; ++v) {
        uint8_t out[4] = {};
        for (int bit = 0; bit < 8; ++bit) {
            if (v & (0x80 >> bit)) out[bit >> 1] |= (uint8_t)((bit & 1) ? 0x0F : 0xF0);
        }
        memcpy(&g_expand[v], out, 4);
    }

    using Draw = void (*)(int, int);
    const Draw draw[kSections] = {ball, text, pan, plasma, strobe, cuts};
    for (int s = 0; s < kSections; ++s) {
        for (int f = 0; f < kList[s].frames; ++f) {
            uint8_t* dst = clip + (size_t)(kList[s].first + f) * kFrameBytes;
            for (int y = 0; y < kH; ++y) {
                draw[s](f, y);
                memcpy(dst + (size_t)y * kStride, g_row, kStride);
            }
        }
    }

    for (int y = 0; y < kH; ++y) {
        rowClear();
        for (int x = kW / 2; x < kW; ++x) shade(x, y, 32);
        memcpy(card + (size_t)y * kStride, g_row, kStride);
    }
    g_card = card;
    g_clip = clip;
    return true;
}

void expand(int i, uint8_t* packed) { expandFrom(g_clip + (size_t)i * kFrameBytes, packed); }

void expandCard(uint8_t* packed) { expandFrom(g_card, packed); }

}  // namespace test_clip
