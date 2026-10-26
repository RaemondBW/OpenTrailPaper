// The e-paper panel in the sim: epd_compat's panel layer (the rasteriser half
// is src/epd_compat.cpp itself). A paint copies the framebuffer to "the
// glass"; screenshots are of the glass, i.e. what the rider would see.
#include "epd_compat.h"
#include "config.h"
#include "sim.h"

#include <atomic>
#include <mutex>
#include <vector>
#include <zlib.h>

namespace {
constexpr int kNativeW = 960, kNativeH = 540, kBytes = kNativeW / 2 * kNativeH;
std::vector<uint8_t> g_fb(kBytes, 0xFF), g_glass(kBytes, 0xFF);
std::mutex g_glassMutex;
std::atomic<uint32_t> g_paints{0};
// Scrubs (epdc_clear_dirty: the scoped resync before a paint) and whole-panel
// clears. Neither changes what the sim's glass shows — residue isn't modelled —
// but the firmware's decision to pay for one is what a test can check.
std::atomic<uint32_t> g_scrubs{0}, g_clears{0};

uint8_t nativeLevel(const std::vector<uint8_t>& fb, int nx, int ny) {
    uint8_t b = fb[(size_t)ny * (kNativeW / 2) + (nx >> 1)];
    return (nx & 1) ? (b >> 4) : (b & 0x0F);
}
}  // namespace

bool epdc_begin() {
    epd_set_rotation(DISPLAY_ROTATION);
    return true;
}
uint8_t* epdc_framebuffer() { return g_fb.data(); }
void epdc_paint() {
    std::lock_guard<std::mutex> lk(g_glassMutex);
    g_glass = g_fb;
    ++g_paints;
}
void epdc_paint_wait() {}
void epdc_power_off_wait() {}
void epdc_park_pins() {}
bool epdc_power_off_verify() { return true; }
void epdc_clear(int) {
    ++g_clears;
    std::lock_guard<std::mutex> lk(g_glassMutex);
    std::fill(g_glass.begin(), g_glass.end(), 0xFF);
}
void epdc_clear_dirty(int) { ++g_scrubs; }
int epdc_grey_levels() { return 16; }

uint8_t* sim::framebuffer() { return g_fb.data(); }
uint32_t sim::paintCount() { return g_paints.load(); }
uint32_t sim::scrubCount() { return g_scrubs.load(); }
uint32_t sim::clearCount() { return g_clears.load(); }

// Portrait, as the rider holds it: drawing (x, y) is native (y, 539 - x) for
// the inverted-portrait rotation the UI uses (epd_compat.cpp).
bool sim::writePng(const std::string& path) {
    const int w = kNativeH, h = kNativeW;
    std::vector<uint8_t> raw((size_t)(w + 1) * h);
    {
        std::lock_guard<std::mutex> lk(g_glassMutex);
        for (int y = 0; y < h; ++y) {
            raw[(size_t)y * (w + 1)] = 0;
            for (int x = 0; x < w; ++x) {
                int nx, ny;
                if (DISPLAY_ROTATION == EPD_ROT_INVERTED_PORTRAIT) { nx = y; ny = kNativeH - 1 - x; }
                else { nx = kNativeW - 1 - y; ny = x; }
                raw[(size_t)y * (w + 1) + 1 + x] = (uint8_t)(nativeLevel(g_glass, nx, ny) * 17);
            }
        }
    }
    uLongf compLen = compressBound(raw.size());
    std::vector<uint8_t> comp(compLen);
    if (compress2(comp.data(), &compLen, raw.data(), raw.size(), 6) != Z_OK) return false;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    auto be32 = [](uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; };
    auto chunk = [&](const char* type, const uint8_t* data, uint32_t len) {
        uint8_t hdr[8];
        be32(hdr, len);
        memcpy(hdr + 4, type, 4);
        fwrite(hdr, 1, 8, f);
        if (len) fwrite(data, 1, len, f);
        uint32_t crc = crc32(0, (const Bytef*)type, 4);
        if (len) crc = crc32(crc, data, len);
        uint8_t c[4];
        be32(c, crc);
        fwrite(c, 1, 4, f);
    };
    const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {};
    be32(ihdr, w);
    be32(ihdr + 4, h);
    ihdr[8] = 8;
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", comp.data(), (uint32_t)compLen);
    chunk("IEND", nullptr, 0);
    fclose(f);
    return true;
}
