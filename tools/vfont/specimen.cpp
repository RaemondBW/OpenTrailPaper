// Host specimen: bitmap Impact/Arial faces beside the vector faces at the
// same digit heights, a weight ramp, and a render-time check.
// Built by tools/vfont/run.sh; writes out/specimen.pgm (+ .png via PIL).
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "epd_compat.h"
#include "ui_render.h"
#include "vfont.h"
#include "fonts/vf/anton.h"
#include "fonts/vf/oswald_400.h"
#include "fonts/vf/oswald_600.h"
#include "fonts/vf/oswald_700.h"
#include "fonts/vf/arimo_400.h"
#include "fonts/vf/arimo_700.h"
#include "fonts/impact_xl.h"
#include "fonts/impact_c.h"
#include "fonts/impact_h.h"
#include "fonts/impact_m.h"
#include "fonts/impact_v.h"
#include "fonts/impact_t.h"
#include "fonts/arial_b.h"
#include "fonts/arialbold_20.h"
#include "map_view.h"
#include "map_tiles.h"

// Link stubs for map_view.cpp (same as tools/preview).
namespace routes {
int pointCount() { return 0; }
void point(int, double& lat, double& lon) { lat = lon = 0; }
}
namespace map_store {
void renderInto(double lat, double lon, float mpp, int cx, int cy, float rot,
                MapScreenData& out, bool (*)(), bool) {
    map_tiles::project(lat, lon, mpp, cx, cy, rot, out);
}
}

constexpr int NATIVE_W = 960, NATIVE_H = 540;   // native landscape 4bpp
constexpr int W = 540, H = 960;                 // portrait drawing space

static void toPortrait(const uint8_t* fb, uint8_t* gray) {
    for (int py = 0; py < H; ++py)
        for (int px = 0; px < W; ++px) {
            int nx = py, ny = NATIVE_H - 1 - px;
            uint8_t b = fb[ny * (NATIVE_W / 2) + nx / 2];
            gray[py * W + px] = ((nx % 2) ? (b >> 4) : (b & 0xF)) * 17;
        }
}

static void label(int x, int y, const char* s, uint8_t* fb) {
    ui::text(&Arial_B, x, y, s, fb);
}

int main(int argc, char** argv) {
    const char* outDir = argc > 1 ? argv[1] : "out";
    std::vector<uint8_t> fb(NATIVE_W / 2 * NATIVE_H, 0xFF);
    std::vector<uint8_t> gray(W * H);
    char path[256];
    epd_set_rotation(EPD_ROT_INVERTED_PORTRAIT);

    // --- Page 1: bitmap vs vector at the ladder's digit heights -------------
    struct Row { const EpdFont* bmp; const char* name; };
    const Row rows[] = {{&Impact_XL, "XL"}, {&Impact_C, "C"}, {&Impact_H, "H"},
                        {&Impact_M, "M"}, {&Impact_V, "V"}, {&Impact_T, "T"}};
    int y = 40;
    label(16, 24, "bitmap Impact  |  Anton  |  Oswald 600   (same '0' ink height)", fb.data());
    for (const Row& r : rows) {
        const int dh = epdc_digit_height(r.bmp);
        y += dh + 14;
        char tag[32]; snprintf(tag, sizeof tag, "%s %dpx", r.name, dh);
        ui::text(r.bmp, 16, y, "27.4", fb.data());
        int x = 16 + ui::textWidth(r.bmp, "27.4") + 16;
        const EpdFont* a = vf_face_digit(&Anton, dh);
        ui::text(a, x, y, "27.4", fb.data());
        x += ui::textWidth(a, "27.4") + 16;
        const EpdFont* o = vf_face_digit(&Oswald_600, dh);
        ui::text(o, x, y, "27.4", fb.data());
        x += ui::textWidth(o, "27.4") + 10;
        label(x, y, tag, fb.data());
        if (y > 700) break;
    }
    // Label faces.
    y += 40;
    label(16, y, "Arial_B bitmap: SPEED  AVG PWR  HEART RATE", fb.data());
    y += 30;
    ui::text(vf_face_cap(&Arimo_700, 14.f), 16, y, "Arimo 700 cap14: SPEED  AVG PWR  HEART RATE", fb.data());
    y += 24;
    ui::text(&ArialBold_20, 16, y, "ArialBold_20: SPEED  AVG PWR", fb.data());
    y += 30;
    ui::text(vf_face_cap(&Arimo_700, 20.f), 16, y, "Arimo 700 cap20: SPEED  AVG PWR", fb.data());
    y += 30;
    ui::text(vf_face_cap(&Arimo_400, 20.f), 16, y, "Arimo 400 cap20: SPEED  AVG PWR", fb.data());

    toPortrait(fb.data(), gray.data());
    snprintf(path, sizeof path, "%s/specimen_compare.pgm", outDir);
    FILE* f = fopen(path, "wb");
    fprintf(f, "P5\n%d %d\n255\n", W, H); fwrite(gray.data(), 1, gray.size(), f); fclose(f);

    // --- Page 2: weight ramp + arbitrary sizes + fit --------------------------
    std::fill(fb.begin(), fb.end(), 0xFF);
    label(16, 24, "Oswald 400 / 600 / 700 at 120 px digit height", fb.data());
    y = 160;
    int x = 16;
    for (const VfFont* w : {&Oswald_400, &Oswald_600, &Oswald_700}) {
        const EpdFont* fce = vf_face_digit(w, 120);
        ui::text(fce, x, y, "18", fb.data());
        x += ui::textWidth(fce, "18") + 24;
    }
    label(16, 200, "Anton, every size from 15 to 95 px in 10 px steps (no ladder)", fb.data());
    y = 320; x = 16;
    for (int px = 15; px <= 95; px += 10) {
        const EpdFont* fce = vf_face_digit(&Anton, px);
        ui::text(fce, x, y, "8", fb.data());
        x += ui::textWidth(fce, "8") + 8;
    }
    label(16, 360, "vf_fit_digit: '1234' into 240x150 with 40 px unit -> largest that fits", fb.data());
    {
        const int cw = 240, ch = 150;
        epd_draw_rect({16, 380, (uint16_t)cw, (uint16_t)ch}, 0x00, fb.data());
        const int px = vf_fit_digit(&Anton, "1234", cw - 20, ch - 20, 40);
        const EpdFont* fce = vf_face_digit(&Anton, px);
        ui::text(fce, 26, 380 + 10 + epdc_digit_height(fce), "1234", fb.data());
        char t[48]; snprintf(t, sizeof t, "-> %d px (digit h %d)", px, epdc_digit_height(fce));
        label(270, 400, t, fb.data());
    }
    label(16, 560, "Oswald 700 small text 12/14/16/18 px em", fb.data());
    y = 590;
    for (float em : {12.f, 14.f, 16.f, 18.f}) {
        ui::text(vf_face(&Oswald_700, em), 16, y, "The quick brown fox 0123456789 km/h", fb.data());
        y += (int)em + 8;
    }
    label(16, 700, "Arimo 700 small text 12/14/16/18 px em", fb.data());
    y = 730;
    for (float em : {12.f, 14.f, 16.f, 18.f}) {
        ui::text(vf_face(&Arimo_700, em), 16, y, "The quick brown fox 0123456789 km/h", fb.data());
        y += (int)em + 8;
    }
    toPortrait(fb.data(), gray.data());
    snprintf(path, sizeof path, "%s/specimen_sizes.pgm", outDir);
    f = fopen(path, "wb");
    fprintf(f, "P5\n%d %d\n255\n", W, H); fwrite(gray.data(), 1, gray.size(), f); fclose(f);

    // --- Timing: cold render of all digits at 158 px, then warm ---------------
    vf_flush();
    auto t0 = std::chrono::steady_clock::now();
    const EpdFont* big = vf_face_digit(&Anton, 158);
    for (char c = '0'; c <= '9'; ++c) { char s[2] = {c, 0}; ui::textWidth(big, s); }
    auto t1 = std::chrono::steady_clock::now();
    ui::text(big, 10, 300, "0123456789", fb.data());
    auto t2 = std::chrono::steady_clock::now();
    printf("cold: 10 digits @158px rasterised in %.2f ms (host); warm blit %.2f ms; cache %zu bytes\n",
           std::chrono::duration<double, std::milli>(t1 - t0).count(),
           std::chrono::duration<double, std::milli>(t2 - t1).count(), vf_cache_bytes());
    return 0;
}
