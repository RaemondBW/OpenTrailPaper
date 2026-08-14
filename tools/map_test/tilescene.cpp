// Renders a real multi-tile map frame on the host, the way map_store::renderInto
// does on the device, and reports what the projector had to throw away.
//
// tools/preview draws ONE .ebm blob (data/sf.ebm). Every scratch-budget and
// tile-selection limit that matters only bites when a dozen-plus H3 tiles are
// projected into the same frame, so the preview could never see them. This tool
// feeds the REAL map_tiles projector and the REAL ui_render_map rasteriser a
// directory of real H3 res-6 tiles.
//
// Usage: ./tilescene <tiledir> <outdir> [lat lon]
//   <tiledir> holds tiles laid out as on the SD card:
//   <tiledir>/<first 6 of h3 id>/<rest>.ebm
//
// Build/run: tools/map_test/run_tilescene.sh

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <zlib.h>

#include <dirent.h>
#include <sys/stat.h>

#include "epd_compat.h"
#include "map_select.h"
#include "map_tiles.h"
#include "map_view.h"
#include "ride_state.h"
#include "ui_render.h"

// map_view.cpp's route-preview path calls these; nothing here has a route.
namespace routes {
int pointCount() { return 0; }
void point(int, double& lat, double& lon) { lat = lon = 0; }
const char* activeName() { return ""; }
}
namespace map_store {
void renderInto(double, double, float, int, int, float, MapScreenData&) {}
}

namespace {

constexpr int NATIVE_W = 960, NATIVE_H = 540;
constexpr int W = 540, H = 960;

struct Tile {
    std::string id;
    std::vector<uint8_t> bytes;
    double s, w, n, e;
};
std::vector<Tile> g_tiles;

template <typename T>
T rd(const uint8_t* p) { T v; memcpy(&v, p, sizeof(T)); return v; }

bool headerBounds(const uint8_t* h, double& s, double& w, double& n, double& e) {
    if (memcmp(h, "EBM2", 4) != 0) return false;
    double lat0 = rd<double>(h + 4), lon0 = rd<double>(h + 12);
    double td = rd<double>(h + 20);
    int32_t nx = rd<int32_t>(h + 28), ny = rd<int32_t>(h + 32);
    s = lat0; w = lon0; n = lat0 + td * ny; e = lon0 + td * nx;
    return true;
}

void loadTile(const std::string& path, const std::string& id) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    Tile t;
    t.id = id;
    t.bytes.resize(sz);
    if (fread(t.bytes.data(), 1, sz, f) != (size_t)sz || sz < 36) { fclose(f); return; }
    fclose(f);
    if (!headerBounds(t.bytes.data(), t.s, t.w, t.n, t.e)) return;
    g_tiles.push_back(std::move(t));
}

// Walk <dir>/<prefix>/<rest>.ebm, mirroring the card layout map_store scans.
void loadTileDir(const char* root) {
    DIR* d = opendir(root);
    if (!d) { fprintf(stderr, "cannot open %s\n", root); return; }
    for (dirent* e = readdir(d); e; e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string sub = std::string(root) + "/" + e->d_name;
        struct stat st;
        if (stat(sub.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            DIR* d2 = opendir(sub.c_str());
            if (!d2) continue;
            for (dirent* f = readdir(d2); f; f = readdir(d2)) {
                if (!strstr(f->d_name, ".ebm")) continue;
                std::string leaf(f->d_name);
                loadTile(sub + "/" + leaf,
                         std::string(e->d_name) + leaf.substr(0, leaf.find(".ebm")));
            }
            closedir(d2);
        } else if (strstr(e->d_name, ".ebm")) {
            std::string leaf(e->d_name);
            loadTile(sub, leaf.substr(0, leaf.find(".ebm")));
        }
    }
    closedir(d);
}

bool writePng(const char* path, const uint8_t* gray, int w, int h) {
    std::vector<uint8_t> raw((size_t)(w + 1) * h);
    for (int y = 0; y < h; ++y) {
        raw[(size_t)y * (w + 1)] = 0;
        memcpy(&raw[(size_t)y * (w + 1) + 1], &gray[(size_t)y * w], w);
    }
    uLongf compLen = compressBound(raw.size());
    std::vector<uint8_t> comp(compLen);
    if (compress2(comp.data(), &compLen, raw.data(), raw.size(), 9) != Z_OK) return false;
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    auto be32 = [](uint8_t* p, uint32_t v) {
        p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
    };
    auto chunk = [&](const char* type, const uint8_t* data, uint32_t len) {
        uint8_t hdr[8];
        be32(hdr, len);
        memcpy(hdr + 4, type, 4);
        fwrite(hdr, 1, 8, f);
        if (len) fwrite(data, 1, len, f);
        uint32_t crc = crc32(0, (const Bytef*)type, 4);
        if (len) crc = crc32(crc, data, len);
        uint8_t crcb[4];
        be32(crcb, crc);
        fwrite(crcb, 1, 4, f);
    };
    const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, w); be32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 0; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", comp.data(), compLen);
    chunk("IEND", nullptr, 0);
    fclose(f);
    return true;
}

void framebufferToPortrait(const uint8_t* fb, uint8_t* gray) {
    for (int py = 0; py < H; ++py) {
        for (int px = 0; px < W; ++px) {
            int nx = py, ny = NATIVE_H - 1 - px;
            uint8_t b = fb[(size_t)ny * (NATIVE_W / 2) + nx / 2];
            uint8_t nib = (nx % 2) ? (b >> 4) : (b & 0xF);
            gray[(size_t)py * W + px] = nib * 17;
        }
    }
}

// "-" when the frame drew everything it had; otherwise which budget bit.
std::string dropSummary(const map_tiles::MapProjectStats& st, int want, int used) {
    std::string s;
    auto add = [&](const char* fmt, int v) {
        if (!v) return;
        char b[64];
        snprintf(b, sizeof(b), fmt, v);
        if (!s.empty()) s += " ";
        s += b;
    };
    add("tiles:%d", want - used);
    add("roads:%d", st.roadsDropped);
    add("water:%d", st.waterDropped);
    add("parks:%d", st.parksDropped);
    add("blobs-cut:%d", st.blobsTruncated);
    add("offscr-w:%d", st.waterOffscreen);
    add("offscr-p:%d", st.parksOffscreen);
    return s.empty() ? "-" : s;
}

RideState sampleState() {
    RideState s;
    s.gpsFix = true;
    s.satellites = 11;
    s.speedKmh = 24.0f;
    s.batteryPercent = 76;
    s.timeValid = true;
    s.utc = 1752182720;
    s.tzMin = -420;
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: tilescene <tiledir> <outdir> [lat lon]\n");
        return 1;
    }
    const char* outdir = argv[2];
    double lat = argc > 4 ? atof(argv[3]) : 37.7749;
    double lon = argc > 4 ? atof(argv[4]) : -122.4194;

    epd_set_rotation(EPD_ROT_INVERTED_PORTRAIT);   // same as the device UI
    loadTileDir(argv[1]);
    if (g_tiles.empty()) { fprintf(stderr, "no tiles loaded\n"); return 1; }
    size_t bytes = 0;
    for (auto& t : g_tiles) bytes += t.bytes.size();
    printf("%zu tiles loaded (%.2f MB)\n\n", g_tiles.size(), bytes / 1048576.0);

    std::vector<uint8_t> fb((size_t)NATIVE_W / 2 * NATIVE_H);
    std::vector<uint8_t> gray((size_t)W * H);
    RideState s = sampleState();

    // The zoom ladder the device actually offers, north-up and track-up.
    const float zooms[] = {2, 4, 8, 16, 32, 64};
    const struct { const char* tag; float rot; } views[] = {
        {"northup", 0.0f}, {"trackup", -37.0f},
    };

    // --sweep: how close does a frame ever get to the scratch budgets? Walks a
    // grid of positions over the whole tile set at every zoom and heading and
    // reports the worst case, which is the only honest way to size MAX_POINTS /
    // MAX_POLYS / the fill budgets in map_tiles.cpp.
    if (getenv("TILESCENE_SWEEP")) {
        double s = 90, w = 180, n = -90, e = -180;
        for (auto& t : g_tiles) {
            s = t.s < s ? t.s : s; n = t.n > n ? t.n : n;
            w = t.w < w ? t.w : w; e = t.e > e ? t.e : e;
        }
        struct { int pts, polys, wpts, wpolys, ppts, ppolys, frames; } peak = {};
        const float rots[] = {0.0f, -20.0f, -45.0f, -70.0f, -90.0f};
        for (int gy = 0; gy <= 12; ++gy) {
            for (int gx = 0; gx <= 12; ++gx) {
                double la = s + (n - s) * gy / 12.0;
                double lo = w + (e - w) * gx / 12.0;
                for (float mpp : zooms) {
                    for (float rot : rots) {
                        int sel[MAP_TILE_BUDGET];
                        int want = 0;
                        int used = mapSelectTiles(
                            (int)g_tiles.size(),
                            [](int i) { return MapTileBox{g_tiles[i].s, g_tiles[i].w,
                                                          g_tiles[i].n, g_tiles[i].e}; },
                            la, lo, mpp, rot, MAP_TILE_BUDGET, sel, &want);
                        MapScreenData m = {};
                        map_tiles::beginProject(m);
                        for (int i = 0; i < used; ++i) {
                            const Tile& t = g_tiles[sel[i]];
                            map_tiles::projectBlobInto(t.bytes.data(), t.bytes.size(),
                                                       la, lo, mpp, 270, 430, rot);
                        }
                        map_tiles::endProject(m);
                        map_tiles::MapProjectStats st = map_tiles::projectStats();
                        peak.frames++;
                        if (st.usedPoints > peak.pts) peak.pts = st.usedPoints;
                        if (m.featureCount > peak.polys) peak.polys = m.featureCount;
                        if (st.usedWaterPoints > peak.wpts) peak.wpts = st.usedWaterPoints;
                        if (m.waterCount > peak.wpolys) peak.wpolys = m.waterCount;
                        if (st.usedParkPoints > peak.ppts) peak.ppts = st.usedParkPoints;
                        if (m.parkCount > peak.ppolys) peak.ppolys = m.parkCount;
                    }
                }
            }
        }
        map_tiles::MapProjectStats cap = map_tiles::projectStats();
        printf("sweep: %d frames over the tile set\n", peak.frames);
        auto row = [](const char* what, int p, int c) {
            printf("  %-13s peak %6d of %6d  (%.0f%% used, %.1fx headroom)\n",
                   what, p, c, 100.0 * p / c, p ? (double)c / p : 0.0);
        };
        row("road points", peak.pts, cap.capPoints);
        row("road polys", peak.polys, cap.capPolys);
        row("water points", peak.wpts, cap.capWaterPoints);
        row("water polys", peak.wpolys, cap.capWaterPolys);
        row("park points", peak.ppts, cap.capParkPoints);
        row("park polys", peak.ppolys, cap.capParkPolys);
        return 0;
    }

    printf("%-9s %-4s %5s %5s  %6s %6s  %5s %5s  %s\n",
           "view", "mpp", "want", "used", "polys", "pts", "wpoly", "wpts", "dropped");
    for (auto& v : views) {
        for (float mpp : zooms) {
            int sel[MAP_TILE_BUDGET];
            int want = 0;
            int used = mapSelectTiles(
                (int)g_tiles.size(),
                [](int i) { return MapTileBox{g_tiles[i].s, g_tiles[i].w,
                                              g_tiles[i].n, g_tiles[i].e}; },
                lat, lon, mpp, v.rot, MAP_TILE_BUDGET, sel, &want);

            MapScreenData map = {};
            map.riderX = 270;
            map.riderY = 430;
            map.metersPerPixel = mpp;
            map.northDeg = v.rot;
            map.trackUp = v.rot != 0;
            map.headingDeg = v.rot != 0 ? 0.0f : 37.0f;
            map.hasMap = true;

            map_tiles::beginProject(map);
            for (int i = 0; i < used; ++i) {
                const Tile& t = g_tiles[sel[i]];
                map_tiles::projectBlobInto(t.bytes.data(), t.bytes.size(), lat, lon,
                                           mpp, map.riderX, map.riderY, v.rot);
            }
            map_tiles::endProject(map);

            map_tiles::MapProjectStats st = map_tiles::projectStats();
            printf("%-9s %-4d %5d %5d  %6d %6d  %5d %5d  %s\n",
                   v.tag, (int)mpp, want, used, map.featureCount, st.usedPoints,
                   map.waterCount, st.usedWaterPoints,
                   dropSummary(st, want, used).c_str());

            memset(fb.data(), 0xFF, fb.size());
            ui_render_map(map, s, fb.data());
            framebufferToPortrait(fb.data(), gray.data());
            char path[256];
            snprintf(path, sizeof(path), "%s/map_%s_mpp%02d.png", outdir, v.tag,
                     (int)mpp);
            writePng(path, gray.data(), W, H);

            // The tap acknowledgement the device paints before it re-projects.
            if (mpp == 8 && v.rot == 0.0f) {
                ui_map_draw_zoom_button(true, true, fb.data());
                framebufferToPortrait(fb.data(), gray.data());
                snprintf(path, sizeof(path), "%s/map_zoom_pressed.png", outdir);
                writePng(path, gray.data(), W, H);
            }
        }
    }
    printf("\nPNGs in %s\n", outdir);
    return 0;
}
