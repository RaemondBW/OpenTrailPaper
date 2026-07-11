// Host preview: renders the device screens through the REAL epdiy drawing
// code and writes PNGs. Usage: ./preview <outdir>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <zlib.h>

#include "epdiy.h"
#include "ride_state.h"
#include "ui_render.h"
#include "map_view.h"
#include "map_tiles.h"

namespace {

constexpr int NATIVE_W = 960, NATIVE_H = 540;
constexpr int W = 540, H = 960;  // portrait

// Minimal 8-bit grayscale PNG writer (zlib for IDAT + CRCs).
bool writePng(const char* path, const uint8_t* gray, int w, int h) {
    std::vector<uint8_t> raw((w + 1) * h);
    for (int y = 0; y < h; ++y) {
        raw[y * (w + 1)] = 0;  // filter: none
        memcpy(&raw[y * (w + 1) + 1], &gray[y * w], w);
    }
    uLongf compLen = compressBound(raw.size());
    std::vector<uint8_t> comp(compLen);
    if (compress2(comp.data(), &compLen, raw.data(), raw.size(), 9) != Z_OK)
        return false;

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
    be32(ihdr, w);
    be32(ihdr + 4, h);
    ihdr[8] = 8;
    ihdr[9] = 0;   // grayscale
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", comp.data(), compLen);
    chunk("IEND", nullptr, 0);
    fclose(f);
    return true;
}

// Unpack the native 4bpp framebuffer into an upright portrait image.
// EPD_ROT_INVERTED_PORTRAIT maps drawing coords (x,y) -> native (y, 539-x).
void framebufferToPortrait(const uint8_t* fb, uint8_t* gray) {
    for (int py = 0; py < H; ++py) {
        for (int px = 0; px < W; ++px) {
            int nx = py;
            int ny = NATIVE_H - 1 - px;
            uint8_t b = fb[ny * (NATIVE_W / 2) + nx / 2];
            uint8_t nib = (nx % 2) ? (b >> 4) : (b & 0xF);
            gray[py * W + px] = nib * 17;
        }
    }
}

void clearWhite(uint8_t* fb) { memset(fb, 0xFF, NATIVE_W / 2 * NATIVE_H); }

RideState sampleState() {
    RideState s;
    s.gpsFix = true;
    s.satellites = 11;
    s.speedKmh = 32.4f;
    s.altitudeM = 612.0f;
    s.heartRateBpm = 156;
    s.powerW = 251;
    s.power3sW = 247;
    s.cadenceRpm = 88;
    s.hrConnected = s.powerConnected = s.cadenceConnected = true;
    s.recording = true;
    s.distanceM = 54800.0;
    s.elapsedS = 1 * 3600 + 47 * 60 + 12;
    s.movingS = 1 * 3600 + 41 * 60 + 2;
    s.gradePercent = 4.2f;
    s.gradeValid = true;
    s.climbedM = 918.0f;
    s.batteryPercent = 76;
    s.timeValid = true;
    // 14:32 PDT == 21:32 UTC on 2026-07-10
    s.utc = 1783805520 - 3600 * 24 * 363;  // arbitrary; clock shows via TZ
    s.utc = 1752182720;                    // ~2025-07-10 21:25 UTC
    s.ftpW = 250;
    s.tzMin = -420;
    return s;
}

RideSummary sampleSummary() {
    RideSummary r;
    r.distanceM = 67400.0;
    r.movingS = 2 * 3600 + 11 * 60 + 38;
    r.elapsedS = r.movingS + 6 * 60;
    r.avgSpeedKmh = 30.7f;
    r.avgPowerW = 231;
    r.normPowerW = 248;
    r.avgHrBpm = 149;
    r.climbedM = 918.0f;
    r.startUtc = 1752182720 - r.elapsedS;
    r.endUtc = 1752182720;
    r.tzMin = -420;
    return r;
}

// --- Synthetic map scene (design 1f look) --------------------------------

struct Scene {
    std::vector<std::vector<int16_t>> lines;
    std::vector<MapPolyline> features;

    void add(MapFeatureClass cls, std::vector<std::pair<float, float>> pts) {
        std::vector<int16_t> flat;
        for (auto& p : pts) {
            flat.push_back((int16_t)lroundf(p.first));
            flat.push_back((int16_t)lroundf(p.second));
        }
        lines.push_back(std::move(flat));
        features.push_back({cls, lines.back().data(),
                            (int)lines.back().size() / 2});
    }
};

void buildScene(Scene& sc) {
    sc.lines.reserve(64);  // keep pts pointers stable across add() calls

    const float rot = 18.0f * (float)M_PI / 180.0f;
    const float c = cosf(rot), s = sinf(rot);
    const float cx = 270, cy = 430;

    auto xy = [&](float gx, float gy) {
        return std::make_pair(cx + gx * c - gy * s, cy + gx * s + gy * c);
    };

    for (int i = -4; i <= 4; ++i) {
        float g = i * 170.0f;
        MapFeatureClass cls = (i % 3 == 0) ? MAP_ROAD_MAJOR : MAP_ROAD_MINOR;
        sc.add(cls, {xy(g, -700), xy(g, 700)});
        sc.add(cls, {xy(-700, g), xy(700, g)});
    }

    std::vector<std::pair<float, float>> river;
    for (int i = 0; i <= 14; ++i) {
        float t = i / 14.0f;
        float gy = -700 + 1400 * t;
        float gx = 480 + 70 * sinf(t * 5.5f);
        river.push_back(xy(gx, gy));
    }
    sc.add(MAP_WATER, river);

    sc.add(MAP_RAIL, {xy(-700, 420), xy(700, 250)});
    sc.add(MAP_PATH, {xy(-170, -170), xy(-40, -60), xy(170, 0)});
}

// Route through the rider: behind (ridden) and ahead with a turn.
std::vector<int16_t> buildRoute(int& riddenPoints) {
    const float rot = 18.0f * (float)M_PI / 180.0f;
    const float c = cosf(rot), s = sinf(rot);
    const float cx = 270, cy = 430;
    auto xy = [&](float gx, float gy) {
        return std::make_pair(cx + gx * c - gy * s, cy + gx * s + gy * c);
    };
    std::vector<std::pair<float, float>> pts = {
        xy(0, 700), xy(0, 200), xy(0, 0),           // ridden (rider at 0,0)
        xy(0, -510), xy(510, -510), xy(700, -510),  // ahead
    };
    riddenPoints = 3;
    std::vector<int16_t> flat;
    for (auto& p : pts) {
        flat.push_back((int16_t)lroundf(p.first));
        flat.push_back((int16_t)lroundf(p.second));
    }
    return flat;
}

}  // namespace

int main(int argc, char** argv) {
    std::string outDir = argc > 1 ? argv[1] : ".";

    epd_init(NULL, &ED047TC1, EPD_OPTIONS_DEFAULT);
    epd_set_rotation(EPD_ROT_INVERTED_PORTRAIT);

    std::vector<uint8_t> fb(NATIVE_W / 2 * NATIVE_H);
    std::vector<uint8_t> gray(W * H);
    RideState s = sampleState();

    auto emit = [&](const char* name) {
        framebufferToPortrait(fb.data(), gray.data());
        std::string p = outDir + "/" + name;
        if (!writePng(p.c_str(), gray.data(), W, H)) {
            fprintf(stderr, "failed to write %s\n", p.c_str());
            exit(1);
        }
        printf("wrote %s\n", p.c_str());
    };

    // Dashboard (1a)
    clearWhite(fb.data());
    ui_render_dashboard(s, false, fb.data());
    emit("dashboard.png");

    // Dashboard while navigating: compact hero under the turn banner.
    clearWhite(fb.data());
    ui_render_dashboard(s, true, fb.data());
    ui_render_nav_banner("Turn left onto Valencia St", 180, false, fb.data());
    emit("dashboard_nav.png");

    // Map view (1f): real SF tiles if the .ebm exists, synthetic otherwise
    MapScreenData map = {};
    map.riderX = 270;
    map.riderY = 430;
    map.headingDeg = -18.0f;
    map.metersPerPixel = 2.0f;

    std::vector<uint8_t> mapBlob;
    FILE* mf = fopen("../../data/sf.ebm", "rb");
    Scene sc;
    std::vector<int16_t> route;
    if (mf) {
        fseek(mf, 0, SEEK_END);
        long sz = ftell(mf);
        fseek(mf, 0, SEEK_SET);
        mapBlob.resize(sz);
        if (fread(mapBlob.data(), 1, sz, mf) != (size_t)sz) return 1;
        fclose(mf);
        if (!map_tiles::load(mapBlob.data(), mapBlob.size())) {
            fprintf(stderr, "sf.ebm failed to parse\n");
            return 1;
        }
        // Alamo Square, zoom 2 m/px (matches device defaults)
        map.northDeg = 0;
        map.trackUp = false;
        map_tiles::project(37.7764, -122.4346, 2.0f, 270, 430, 0, map);
        printf("sf.ebm: %d visible polylines\n", map.featureCount);
    } else {
        buildScene(sc);
        int ridden = 0;
        route = buildRoute(ridden);
        map.features = sc.features.data();
        map.featureCount = (int)sc.features.size();
        map.route = route.data();
        map.routePointCount = (int)route.size() / 2;
        map.riddenPointCount = ridden;
    }

    clearWhite(fb.data());
    ui_render_map(map, s, fb.data());
    emit("map.png");

    // Zoomed-out view of the same spot
    if (mf) {
        map_tiles::project(37.7764, -122.4346, 8.0f, 270, 430, 0, map);
        map.metersPerPixel = 8.0f;
        clearWhite(fb.data());
        ui_render_map(map, s, fb.data());
        emit("map_zoom8.png");

        // Track-up: heading 40 deg, world rotated -40
        map_tiles::project(37.7764, -122.4346, 2.0f, 270, 430, -40, map);
        map.metersPerPixel = 2.0f;
        map.headingDeg = 0;
        map.northDeg = -40;
        map.trackUp = true;
        clearWhite(fb.data());
        ui_render_map(map, s, fb.data());
        emit("map_trackup.png");
    }

    // Ride summary (1g)
    clearWhite(fb.data());
    ui_render_summary(sampleSummary(), fb.data());
    emit("summary.png");

    // Menu (1h)
    MenuInfo menu;
    menu.recording = false;
    menu.gpsReady = true;
    menu.sdOk = true;
    menu.rideCount = 42;
    menu.sdFreeMB = 29876;
    menu.hr = menu.pwr = true;
    menu.cad = false;
    menu.batteryPercent = 78;
    snprintf(menu.routeLine, sizeof(menu.routeLine), "coastal.gpx · 26.4 km left");
    clearWhite(fb.data());
    ui_render_menu(menu, fb.data());
    emit("menu.png");

    // Sensors list
    ListRow sensors[3] = {};
    snprintf(sensors[0].title, sizeof(sensors[0].title), "WHOOP 4.0");
    snprintf(sensors[0].subtitle, sizeof(sensors[0].subtitle),
             "heart rate · -58 dBm · paired · connected");
    sensors[0].inverted = true;
    snprintf(sensors[1].title, sizeof(sensors[1].title), "Assioma DUO");
    snprintf(sensors[1].subtitle, sizeof(sensors[1].subtitle),
             "power + cadence · -66 dBm · paired");
    snprintf(sensors[2].title, sizeof(sensors[2].title), "f0:99:1c:22:8a:01");
    snprintf(sensors[2].subtitle, sizeof(sensors[2].subtitle),
             "cadence · -80 dBm");
    clearWhite(fb.data());
    ui_render_list("SENSORS", sensors, 3, "tap a sensor to pair it · scanning...",
                   fb.data());
    emit("sensors.png");

    // Routes list
    ListRow routes[3] = {};
    snprintf(routes[0].title, sizeof(routes[0].title), "Clear route");
    snprintf(routes[0].subtitle, sizeof(routes[0].subtitle),
             "coastal.gpx · 26.4 km left");
    routes[0].inverted = true;
    snprintf(routes[1].title, sizeof(routes[1].title), "coastal.gpx");
    snprintf(routes[1].subtitle, sizeof(routes[1].subtitle),
             "tap to ride this route");
    snprintf(routes[2].title, sizeof(routes[2].title), "mt_tam_loop.gpx");
    snprintf(routes[2].subtitle, sizeof(routes[2].subtitle),
             "tap to ride this route");
    clearWhite(fb.data());
    ui_render_list("NAVIGATE", routes, 3,
                   "put .gpx files in /routes on the SD card", fb.data());
    emit("routes.png");

    // Settings
    SettingsInfo si{250, -420, 2};
    clearWhite(fb.data());
    ui_render_settings(si, fb.data());
    emit("settings.png");

    // GPS debug: the "sees satellites but no fix" case
    GpsDebugView g = {};
    g.moduleDetected = true;
    g.chars = 184223;
    g.passedCksum = 2141;
    g.failedCksum = 3;
    g.withFix = 0;
    g.satsInView = 7;
    g.satsInUse = 0;
    g.hdop = 0;
    g.locValid = false;
    g.hour = 21;
    g.minute = 48;
    g.second = 12;
    clearWhite(fb.data());
    ui_render_gps_debug(g, fb.data());
    emit("gpsdebug.png");

    // Power sheet drawn over the dashboard (overlay behavior)
    clearWhite(fb.data());
    ui_render_dashboard(s, false, fb.data());
    ui_render_power_sheet(true, fb.data());
    emit("power.png");

    // Nav prompt over the map
    clearWhite(fb.data());
    ui_render_map(map, s, fb.data());
    ui_render_nav_prompt("mission_dolores.gpx", 8, fb.data());
    emit("nav_prompt.png");

    // Turn-by-turn banner over the map
    clearWhite(fb.data());
    ui_render_map(map, s, fb.data());
    ui_render_nav_banner("Turn left onto Valencia St", 180, false, fb.data());
    emit("nav_banner.png");

    return 0;
}
