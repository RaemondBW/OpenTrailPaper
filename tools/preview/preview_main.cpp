// Host preview: renders the device screens through the REAL epdiy drawing
// code and writes PNGs. Usage: ./preview <outdir>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <strings.h>
#include <string>
#include <utility>
#include <vector>
#include <zlib.h>

// epd_compat.h rather than epdiy.h: the preview must exercise the SAME
// rasteriser the device runs, or it stops being evidence. It caught two layout
// bugs against epdiy's renderer; pointed at a different renderer than ships, the
// next one it "catches" could be its own.
#include "epd_compat.h"
#include "ride_state.h"
#include "ui_render.h"
#include "media_state.h"
#include "workout.h"
#include "stopwatch.h"
#include "segment_match.h"
#include "map_view.h"
#include "map_tiles.h"

// The routes:: symbols map_view.cpp references for the route preview. These
// used to be empty stubs on the grounds that "the preview never has an active
// route" — which quietly made the accept page a lie: ui_render_route_preview
// returns immediately without points, so nav_prompt.png was rendered over the
// ordinary map view and showed the rider's own neighbourhood rather than the
// route they were being asked to accept. The screen sets a real one.
namespace routes {
std::vector<std::pair<double, double>> gRoute;
int pointCount() { return (int)gRoute.size(); }
void point(int i, double& lat, double& lon) {
    lat = gRoute[i].first;
    lon = gRoute[i].second;
}
const char* activeName() { return "mission_dolores.gpx"; }
}

// map_store is device-only (SD-backed tile store). On the host, forward the
// route-preview road-context render to map_tiles' single loaded blob.
namespace map_store {
void renderInto(double lat, double lon, float mpp, int cx, int cy, float rot,
                MapScreenData& out, bool (*)(), bool) {
    map_tiles::project(lat, lon, mpp, cx, cy, rot, out);
}
}

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
    s.phoneConnected = true;
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
    const RideState s = sampleState();
    RideSummary r;
    r.distanceM = s.distanceM;
    r.movingS = s.movingS;
    r.elapsedS = s.elapsedS;
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
    std::vector<std::vector<int16_t>> waterLines;
    std::vector<MapPolyline> water;

    void add(MapFeatureClass cls, std::vector<std::pair<float, float>> pts) {
        std::vector<int16_t> flat;
        for (auto& p : pts) {
            flat.push_back((int16_t)lroundf(p.first));
            flat.push_back((int16_t)lroundf(p.second));
        }
        lines.push_back(std::move(flat));
        features.push_back({cls, 0, lines.back().data(),
                            (int)lines.back().size() / 2});
    }

    void addWater(std::vector<std::pair<float, float>> pts) {
        std::vector<int16_t> flat;
        for (auto& p : pts) {
            flat.push_back((int16_t)lroundf(p.first));
            flat.push_back((int16_t)lroundf(p.second));
        }
        waterLines.push_back(std::move(flat));
        water.push_back({MAP_WATER, 0, waterLines.back().data(),
                         (int)waterLines.back().size() / 2});
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

    // Water body (a lake) as a CLOSED polygon so the renderer's dithered fill
    // has something to test — placed on-screen (upper-left of the viewport).
    sc.addWater({xy(-250, -360), xy(-20, -330), xy(40, -140), xy(-90, -40),
                 xy(-260, -110), xy(-320, -250)});

    // Secondary road (new tier) in place of the removed rail line.
    sc.add(MAP_ROAD_SECONDARY, {xy(-700, 420), xy(700, 250)});
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

    // No epd_init(): the compat rasteriser has no hardware state to bring up, and
    // rotation is the only thing the old call was here for.
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
    ui_render_dashboard(s, false, dashDefaultLayout(), fb.data());
    emit("dashboard.png");

    // Radar reference: one full-height rail beside ordinary ride fields.
    {
        DashLayout radarLayout;
        dashParse("speed large\npower3s medium\nhr medium\nridetime medium\nradar medium vertical\n", radarLayout);
        RideState st = s;
        st.showOffline = true;
        st.radar.connected = st.radar.received = st.radar.live = true;
        st.radar.count = 3;
        st.radar.targets[0] = {1, 28, 1000};
        st.radar.targets[1] = {2, 70, 1000};
        st.radar.targets[2] = {3, 118, 1000};
        clearWhite(fb.data()); ui_render_dashboard(st, false, radarLayout, fb.data()); emit("radar_traffic.png");
        radarLayout.items[4].heightPercent = 50;
        clearWhite(fb.data()); ui_render_dashboard(st, false, radarLayout, fb.data()); emit("radar_half.png");
        radarLayout.items[4].heightPercent = 75;
        clearWhite(fb.data()); ui_render_dashboard(st, false, radarLayout, fb.data()); emit("radar_three_quarter.png");
        radarLayout.items[4].heightPercent = 100;
        st.useMiles = true;
        clearWhite(fb.data()); ui_render_dashboard(st, false, radarLayout, fb.data()); emit("radar_imperial.png");
        st.useMiles = false;
        DashLayout heroRadar;
        dashParse("power3s hero\nhr medium\nridetime medium\nradar medium\n", heroRadar);
        clearWhite(fb.data()); ui_render_dashboard(st, false, heroRadar, fb.data()); emit("radar_power_hero.png");
        // A full-height radar reserves this gutter for the entire page. The
        // power-zone bar must not draw a single pixel outside its hero cell.
        for (int y = 76; y < 936; ++y) for (int x = 340; x < 352; ++x) {
            if (gray[y * W + x] != 255) {
                fprintf(stderr, "Hero power drawing escaped into radar gutter at %d,%d\n", x, y);
                exit(1);
            }
        }
        heroRadar.items[3].heightPercent = 50;
        clearWhite(fb.data()); ui_render_dashboard(st, false, heroRadar, fb.data()); emit("radar_power_hero_half.png");
        heroRadar.items[3].bottomAligned = true;
        clearWhite(fb.data()); ui_render_dashboard(st, false, heroRadar, fb.data()); emit("radar_power_hero_bottom.png");
        clearWhite(fb.data()); ui_render_dashboard(st, true, heroRadar, fb.data());
        ui_render_nav_banner("LEFT ONTO QUEENSFERRY RD", 400, false, fb.data());
        emit("radar_power_hero_bottom_nav.png");
        st.radar.count = 8;
        for (int i = 0; i < 8; ++i) st.radar.targets[i] = {uint8_t(i), uint8_t(21 + i * 2), 1000};
        clearWhite(fb.data()); ui_render_dashboard(st, true, radarLayout, fb.data());
        ui_render_nav_banner("LEFT ONTO QUEENSFERRY RD", 400, false, fb.data());
        emit("radar_crowded_nav.png");
        radarLayout.items[4].heightPercent = 50;
        clearWhite(fb.data()); ui_render_dashboard(st, true, radarLayout, fb.data());
        ui_render_nav_banner("LEFT ONTO QUEENSFERRY RD", 400, false, fb.data());
        emit("radar_half_crowded_nav.png");
        st.radar.targets[0].distanceM = 0;
        st.radar.targets[1].distanceM = 150;
        st.radar.count = 2;
        st.useMiles = true;
        clearWhite(fb.data()); ui_render_dashboard(st, true, radarLayout, fb.data());
        ui_render_nav_banner("LEFT ONTO QUEENSFERRY RD", 400, false, fb.data());
        emit("radar_half_range_nav.png");
        st.useMiles = false;
        st.radar.live = false;
        clearWhite(fb.data()); ui_render_dashboard(st, true, radarLayout, fb.data());
        ui_render_nav_banner("LEFT ONTO QUEENSFERRY RD", 400, false, fb.data());
        emit("radar_half_no_signal_nav.png");
        st.radar.live = true;
        radarLayout.items[4].heightPercent = 100;
        st.radar.count = 0;
        clearWhite(fb.data()); ui_render_dashboard(st, false, radarLayout, fb.data()); emit("radar_clear.png");
        DashLayout numeric;
        dashParse("speed large\npower3s medium\nhr medium\nridetime medium vertical\n", numeric);
        clearWhite(fb.data()); ui_render_dashboard(st, false, numeric, fb.data()); emit("dashboard_vertical.png");
        st.radar.live = false;
        clearWhite(fb.data()); ui_render_dashboard(st, false, radarLayout, fb.data()); emit("radar_no_signal.png");
        st.radar.connected = false;
        st.showOffline = false;
        clearWhite(fb.data()); ui_render_dashboard(st, false, radarLayout, fb.data()); emit("radar_offline.png");
    }

    // Dashboard in imperial units, no power meter (speed hero)
    {
        RideState sm = s;
        sm.useMiles = true;
        sm.powerConnected = false;
        clearWhite(fb.data());
        ui_render_dashboard(sm, false, dashDefaultLayout(), fb.data());
        emit("dashboard_mph.png");
    }

    // Dashboard auto-paused at a stop: banner in the turn-banner band (compact
    // hero, like navigating), frozen timer, and the status-bar PAUSED chip.
    // HR/PWR connected is the crowded status-bar case — catches a chip
    // collision with the battery block.
    {
        RideState sp = s;
        sp.ridePaused = true;
        sp.speedKmh = 0.0f;
        clearWhite(fb.data());
        ui_render_dashboard(sp, true, dashDefaultLayout(), fb.data());
        ui_render_pause_banner(fb.data());
        emit("dashboard_paused.png");
    }

    // Dashboard while navigating: compact hero under the turn banner.
    clearWhite(fb.data());
    ui_render_dashboard(s, true, dashDefaultLayout(), fb.data());
    ui_render_nav_banner("Turn left onto Valencia St", 180, false, fb.data());
    emit("dashboard_nav.png");

    // Navigating AND auto-paused: the turn banner wins the band, the pause
    // lives in the status bar chip — a rider stopped AT a turn still sees
    // the instruction.
    {
        RideState sp = s;
        sp.ridePaused = true;
        sp.speedKmh = 0.0f;
        clearWhite(fb.data());
        ui_render_dashboard(sp, true, dashDefaultLayout(), fb.data());
        ui_render_nav_banner("Turn left onto Valencia St", 180, false, fb.data());
        emit("dashboard_nav_paused.png");
    }

    // Auto-paused on the music and workout pages: no banner — the page keeps
    // its content and the status bar carries the pause ("MUSIC · PAUSED").
    {
        RideState sp = s;
        sp.ridePaused = true;
        sp.speedKmh = 0.0f;
        MediaState mp;
        mp.present = true;
        mp.playing = false;
        mp.posSec = 154;
        mp.durSec = 331;
        snprintf(mp.title, sizeof(mp.title), "Turn Your Lights Down Low");
        snprintf(mp.artist, sizeof(mp.artist), "Bob Marley & The Wailers");
        snprintf(mp.album, sizeof(mp.album), "Exodus");
        clearWhite(fb.data());
        ui_render_media(sp, mp, fb.data());
        emit("music_ride_paused.png");

        static Workout pwk;
        static const char* kPErg =
            "[COURSE HEADER]\nMINUTES WATTS\n[END COURSE HEADER]\n"
            "[COURSE DATA]\n0 120\n10 180\n10 260\n15 260\n15 140\n18 140\n"
            "18 260\n23 260\n[END COURSE DATA]\n";
        workoutParse(kPErg, 250, pwk);
        snprintf(pwk.name, sizeof(pwk.name), "4X5 THRESHOLD");
        WorkoutView pv;
        sp.powerConnected = true;
        sp.power3sW = 258;
        workoutBuildView(pwk, 20 * 60, true, 250, pv);
        clearWhite(fb.data());
        ui_render_workout(sp, pv, fb.data());
        emit("workout_ride_paused.png");
    }

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
        map.metersPerPixel = 2.0f;
        map_tiles::project(37.7764, -122.4346, 2.0f, 270, 430, 0, map);
        int cc[7]; map_tiles::projectedClassCounts(cc);
        printf("sf.ebm: %d polylines, %d water polys, %d park polys\n",
               map.featureCount, map.waterCount, map.parkCount);
        printf("  kept by class: art=%d pri=%d sec=%d ter=%d min=%d path=%d\n",
               cc[0], cc[1], cc[2], cc[3], cc[4], cc[5]);
    } else {
        buildScene(sc);
        int ridden = 0;
        route = buildRoute(ridden);
        map.features = sc.features.data();
        map.featureCount = (int)sc.features.size();
        map.water = sc.water.data();
        map.waterCount = (int)sc.water.size();
        map.route = route.data();
        map.routePointCount = (int)route.size() / 2;
        map.riddenPointCount = ridden;
    }

    clearWhite(fb.data());
    ui_render_map(map, s, fb.data());
    emit("map.png");

    // No-map-coverage state (nothing downloaded for this position).
    {
        MapScreenData empty = map;
        empty.featureCount = 0;
        empty.hasMap = false;
        clearWhite(fb.data());
        ui_render_map(empty, s, fb.data());
        emit("map_nomap.png");
        // Same spot with no card: downloaded maps live on it, so the prompt
        // says what is actually missing.
        RideState ns = s;
        ns.sdCard = SD_CARD_NONE;
        clearWhite(fb.data());
        ui_render_map(empty, ns, fb.data());
        emit("map_no_sd.png");
        ns.sdCard = SD_CARD_UNREADABLE;
        clearWhite(fb.data());
        ui_render_map(empty, ns, fb.data());
        emit("map_sd_unreadable.png");
    }

    // Powered-off screen: full-screen map backdrop (max zoom-out) + plate
    if (mf) {
        map_tiles::project(37.765, -122.435, 32.0f, 270, 430, 0, map);
        map.metersPerPixel = 32.0f;
    }
    clearWhite(fb.data());
    ui_render_map_features(map, s, fb.data());
    ui_render_shutdown_screen(fb.data());
    emit("powered_off.png");

    // Shutdown tap acknowledgment band (instant feedback before the slow save)
    clearWhite(fb.data());
    ui_render_dashboard(s, false, dashDefaultLayout(), fb.data());
    ui_render_shutdown_ack(true, fb.data());
    emit("shutdown_ack.png");

    // The same band as it really lands on the glass after SHUT DOWN is tapped:
    // shutdownDevice() paints it straight over the frame still in the buffer,
    // which is the power sheet and its scrim over the dashboard.
    clearWhite(fb.data());
    ui_render_dashboard(s, false, dashDefaultLayout(), fb.data());
    ui_render_power_sheet(true, fb.data());
    ui_render_shutdown_ack(true, fb.data());
    emit("power_off_ack.png");

    // Ride save/discard acknowledgment over the summary sheet
    clearWhite(fb.data());
    ui_render_dashboard(s, false, dashDefaultLayout(), fb.data());
    ui_render_summary(sampleSummary(), fb.data());
    ui_render_busy_band("DISCARDING RIDE", "one moment...", fb.data(), 440, 180);
    emit("ride_ack.png");

    // Zoomed-out overview: whole SF, ocean (west) + bay (east) both in frame.
    if (mf) {
        map_tiles::project(37.765, -122.435, 32.0f, 270, 430, 0, map);
        map.metersPerPixel = 32.0f;
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
    // Modal: draw the ride screen behind it, as the device does.
    ui_render_dashboard(s, false, dashDefaultLayout(), fb.data());
    ui_render_summary(sampleSummary(), fb.data());
    emit("summary.png");

    // Interrupted-ride recovery prompt (boot): same sheet, different question.
    {
        RideSummary rs = sampleSummary();
        clearWhite(fb.data());
        ui_render_summary(rs, fb.data(), true);
        emit("summary_recovery.png");
    }

    // Firmware update modal
    clearWhite(fb.data());
    ui_render_update_overlay("Downloading", 100, fb.data());
    emit("update.png");

    // Status bar: 12-hour clock with the phone connected. "12:45p" is a
    // character wider than "14:25" and used to run into the phone glyph.
    {
        RideState sb = s;
        sb.phoneConnected = true;
        sb.hrConnected = true;
        sb.powerConnected = true;
        sb.clock24h = false;
        clearWhite(fb.data());
        ui_render_dashboard(sb, false, dashDefaultLayout(), fb.data());
        emit("statusbar_12h.png");
        sb.clock24h = true;
        clearWhite(fb.data());
        ui_render_dashboard(sb, false, dashDefaultLayout(), fb.data());
        emit("statusbar_24h.png");
        // No SD card: the chip leads the cluster, ahead of the sensors, and
        // still leaves the battery % clear with HR + PWR connected.
        sb.sdCard = SD_CARD_NONE;
        clearWhite(fb.data());
        ui_render_dashboard(sb, false, dashDefaultLayout(), fb.data());
        emit("statusbar_no_sd.png");
    }

    // Boot progress — mid-boot, and the failure case that matters most
    {
        // Nine steps, in the order main.cpp announces them: the Qwiic sensors get
        // a line each when a board is attached, and the mesh radio adds one more.
        // This is the longest the list gets, so it is what says whether the boot
        // screen still fits — and the COMPASS row is the not-found case, which is
        // the one worth being able to read.
        const char*  steps[] = {"SD CARD", "RTC", "GPS", "MESH", "BARO", "MOTION",
                                "COMPASS", "DISPLAY", "MAPS"};
        const char det[][28] = {"30436 MB free · 42 rides", "clock restored",
                                "CASIC · aiding sent", "LongFast 906.875 MHz",
                                "BME280 @0x77", "LSM303AGR @0x19",
                                "not found @0x1E", "epd_painter 540x960",
                                "107 tiles · indexed"};
        const uint32_t ms[] = {1870, 2240, 4910, 5120, 5190, 5210, 5230, 5240, 8020};
        const int8_t allOk[] = {BOOT_OK, BOOT_OK, BOOT_OK, BOOT_OK, BOOT_OK,
                                BOOT_OK, BOOT_FAIL, BOOT_OK, BOOT_OK};
        const int8_t sdBad[] = {BOOT_FAIL, BOOT_OK, BOOT_OK, BOOT_OK, BOOT_OK,
                                BOOT_OK, BOOT_OK, BOOT_OK, BOOT_OK};
        // Mid-mount: the SD step announced but not yet resolved. This is the
        // state the device sits in for a second or two on a cold card, so it is
        // worth eyeballing that the ring lines up with the ticks above it.
        const int8_t sdBusy[] = {BOOT_PENDING};
        // Indexing the card: the LONGEST pending state of a boot (~4 s), and
        // the one that used to show no pending step at all.
        const int8_t mapBusy[] = {BOOT_OK, BOOT_OK, BOOT_OK, BOOT_OK, BOOT_OK,
                                  BOOT_OK, BOOT_OK, BOOT_OK, BOOT_PENDING};
        clearWhite(fb.data());
        ui_render_boot_screen("v0.91", steps, allOk, det, ms, 9, fb.data());
        emit("boot.png");
        // The SD line carries what main.cpp stages for each failure: an empty
        // slot, or a card that answered but would not mount (exFAT).
        char badDet[9][28];
        memcpy(badDet, det, sizeof(badDet));
        snprintf(badDet[0], sizeof(badDet[0]), "No SD card");
        clearWhite(fb.data());
        ui_render_boot_screen("v0.91", steps, sdBad, badDet, ms, 9, fb.data());
        emit("boot_sd_failed.png");
        snprintf(badDet[0], sizeof(badDet[0]), "unreadable · FAT32?");
        clearWhite(fb.data());
        ui_render_boot_screen("v0.91", steps, sdBad, badDet, ms, 9, fb.data());
        emit("boot_sd_unreadable.png");
        clearWhite(fb.data());
        ui_render_boot_screen("v0.91", steps, sdBusy, det, ms, 1, fb.data());
        emit("boot_sd_mounting.png");
        clearWhite(fb.data());
        ui_render_boot_screen("v0.91", steps, mapBusy, det, ms, 9, fb.data());
        emit("boot_map_indexing.png");
    }

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
    menu.rideDistanceM = s.distanceM;
    menu.rideElapsedS = s.elapsedS;
    snprintf(menu.routeLine, sizeof(menu.routeLine), "coastal.gpx · 26.4 km left");
    clearWhite(fb.data());
    ui_render_menu(menu, fb.data());
    ui::statusBar(s, fb.data(), "MENU");
    emit("menu.png");

    // No card: Start Ride says why it won't record, History and the footer
    // already said "no SD card".
    {
        MenuInfo nm = menu;
        nm.sdOk = false;
        RideState ns = s;
        ns.sdCard = SD_CARD_NONE;
        clearWhite(fb.data());
        ui_render_menu(nm, fb.data());
        ui::statusBar(ns, fb.data(), "MENU");
        emit("menu_no_sd.png");
    }

    // The same menu during a recorded ride. The website walkthrough uses this
    // state after leaving sensor pairing so END RIDE can lead to the summary.
    menu.recording = true;
    clearWhite(fb.data());
    ui_render_menu(menu, fb.data());
    ui::statusBar(s, fb.data(), "MENU");
    emit("menu_recording.png");

    // PAIRED DEVICES (was SENSORS): the phone first, then sensor candidates.
    // Mirrors renderListScreen's row recipe in ui_dashboard.cpp.
    ListRow sensors[4] = {};
    snprintf(sensors[0].title, sizeof(sensors[0].title), "Raemond's iPhone");
    snprintf(sensors[0].subtitle, sizeof(sensors[0].subtitle),
             "Connected · Phone · tap to unpair");
    sensors[0].inverted = true;
    snprintf(sensors[1].title, sizeof(sensors[1].title), "WHOOP 4.0");
    snprintf(sensors[1].subtitle, sizeof(sensors[1].subtitle),
             "Connected · HR");
    sensors[1].inverted = true;
    snprintf(sensors[2].title, sizeof(sensors[2].title), "Assioma DUO");
    snprintf(sensors[2].subtitle, sizeof(sensors[2].subtitle),
             "Saved · Power+Cad");
    snprintf(sensors[3].title, sizeof(sensors[3].title), "f0:99:1c:22:8a:01");
    snprintf(sensors[3].subtitle, sizeof(sensors[3].subtitle),
             "Cadence · -80 dBm");
    clearWhite(fb.data());
    ui_render_list("PAIRED DEVICES", sensors, 4, "tap a sensor to pair it · scanning...",
                   fb.data());
    // ui_render_list does NOT draw the title it is handed — the header is the
    // status bar, which ui_dashboard draws over every list screen. Leaving it
    // out here is why these screens looked headerless on the site.
    ui::statusBar(s, fb.data(), "PAIRED DEVICES");
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
                   "put .gpx files in /routes on the SD card", fb.data(), false,
                   true);
    ui_render_pager(0, 3, 3, "REFRESH", fb.data());
    ui::statusBar(s, fb.data(), "NAVIGATE");
    emit("routes.png");

    // Navigate and Ride History page five rows at a time, the same pager
    // footer as the workout picker. One "page" helper builds what
    // renderListScreen() does: pinned rows first, then the files.
    {
        auto pagedList = [&](const char* title, const char* footer,
                             const ListRow* all, int total, int pg,
                             const char* file) {
            ListRow rows[kMenuRowCount] = {};
            int count = 0;
            for (int i = pg * kMenuRowCount; i < total && count < kMenuRowCount;
                 ++i)
                rows[count++] = all[i];
            clearWhite(fb.data());
            ui_render_list(title, rows, count, footer, fb.data(), false, true);
            ui_render_pager(pg * kMenuRowCount, count, total, "REFRESH",
                            fb.data());
            ui::statusBar(s, fb.data(), title);
            emit(file);
        };

        // Navigate: an active route (pinned) and 12 GPX files, three pages.
        static ListRow nav[13] = {};
        nav[0] = routes[0];
        const char* gpx[] = {"coastal.gpx", "mt_tam_loop.gpx",
                             "paradise_loop.gpx", "old_la_honda.gpx",
                             "kings_mountain.gpx", "tunitas_creek.gpx",
                             "skyline_out_back.gpx", "half_moon_bay.gpx",
                             "pescadero_loop.gpx", "mount_diablo.gpx",
                             "sunol_regional.gpx", "calaveras_road.gpx"};
        for (int i = 0; i < 12; ++i) {
            snprintf(nav[i + 1].title, sizeof(nav[0].title), "%s", gpx[i]);
            snprintf(nav[i + 1].subtitle, sizeof(nav[0].subtitle),
                     "tap to ride this route");
        }
        const char* navFoot = "put .gpx files in /routes on the SD card";
        pagedList("NAVIGATE", navFoot, nav, 13, 0, "routes_paged_p1.png");
        pagedList("NAVIGATE", navFoot, nav, 13, 1, "routes_paged_p2.png");
        pagedList("NAVIGATE", navFoot, nav, 13, 2, "routes_paged_p3.png");
        pagedList("NAVIGATE", "no SD card", nav, 0, 0, "routes_empty.png");

        // Ride History: a ride recording (pinned) and 71 finished rides,
        // newest first — 72 rows, 15 pages.
        static ListRow rides[72] = {};
        snprintf(rides[0].title, sizeof(rides[0].title),
                 "Recording in progress");
        snprintf(rides[0].subtitle, sizeof(rides[0].subtitle),
                 "24.6 km · 1:02:37");
        rides[0].inverted = true;
        for (int i = 1; i < 72; ++i) {
            int day = 30 - (i - 1) % 30, month = 10 - (i - 1) / 30;
            snprintf(rides[i].title, sizeof(rides[0].title),
                     "2026%02d%02d-07%02d12.fit", month, day, (i * 7) % 60);
            snprintf(rides[i].subtitle, sizeof(rides[0].subtitle), "%d KB",
                     40 + (i * 37) % 260);
        }
        const char* rideFoot = "rides upload from /rides on the SD card";
        pagedList("RIDES", rideFoot, rides, 72, 0, "history_p1.png");
        pagedList("RIDES", rideFoot, rides, 72, 7, "history_p8.png");
        pagedList("RIDES", rideFoot, rides, 72, 14, "history_p15.png");
        // Not riding, three rides: one page, both arrows disabled.
        pagedList("RIDES", rideFoot, rides + 1, 3, 0, "history_single.png");
    }

    // Settings
    // useMiles ON deliberately: "MILES" is the longest word either switch has to
    // carry and the reason the UNITS control is wider than the rest, so the
    // preview has to render that position rather than the short "KM" one.
    SettingsInfo si{true, 250, -420, 2, true, true, true};
    clearWhite(fb.data());
    ui_render_settings(si, fb.data());
    ui::statusBar(s, fb.data(), "SETTINGS");
    emit("settings.png");

    // MAP LAYERS (from the layers button on the map): everything on, and a
    // rider who keeps water + repair only.
    clearWhite(fb.data());
    ui_render_map_layers(MAP_LAYERS_ALL, fb.data());
    ui::statusBar(s, fb.data(), "MAP LAYERS");
    emit("map_layers.png");
    clearWhite(fb.data());
    ui_render_map_layers(MAP_LAYER_WATER | MAP_LAYER_REPAIR, fb.data());
    ui::statusBar(s, fb.data(), "MAP LAYERS");
    emit("map_layers_some_off.png");

    // Fixed pairing: the phone row on PAIRED DEVICES, paired and not, the
    // unpair sheet over it (and its migrated / not-paired variants), and the
    // main menu's Paired Devices subtitle.
    {
        auto pairedPage = [&](bool paired) {
            ListRow rows[3] = {};
            if (paired) {
                snprintf(rows[0].title, sizeof(rows[0].title), "Raemond's iPhone");
                snprintf(rows[0].subtitle, sizeof(rows[0].subtitle),
                         "Connected · Phone · tap to unpair");
                rows[0].inverted = true;
            } else {
                snprintf(rows[0].title, sizeof(rows[0].title), "Phone");
                snprintf(rows[0].subtitle, sizeof(rows[0].subtitle),
                         "Not paired · open the app to pair");
            }
            snprintf(rows[1].title, sizeof(rows[1].title), "WHOOP 4.0");
            snprintf(rows[1].subtitle, sizeof(rows[1].subtitle), "Connected · HR");
            rows[1].inverted = true;
            snprintf(rows[2].title, sizeof(rows[2].title), "Assioma DUO");
            snprintf(rows[2].subtitle, sizeof(rows[2].subtitle), "Saved · Power+Cad");
            clearWhite(fb.data());
            ui_render_list("PAIRED DEVICES", rows, 3,
                           "tap a sensor to pair it · scanning...", fb.data());
            ui::statusBar(s, fb.data(), "PAIRED DEVICES");
        };
        pairedPage(true);
        emit("paired_devices.png");
        pairedPage(true);
        ui_render_unpair_sheet(true, "Raemond's iPhone", 1, fb.data());
        emit("unpair_sheet.png");
        pairedPage(true);
        ui_render_unpair_sheet(true, "", 2, fb.data());
        emit("unpair_sheet_migrated.png");
        pairedPage(false);
        emit("paired_devices_unpaired.png");
        pairedPage(false);
        ui_render_unpair_sheet(false, "", 0, fb.data());
        emit("unpair_sheet_not_paired.png");

        MenuInfo mp = menu;
        mp.recording = false;
        mp.phonePaired = true;
        snprintf(mp.phoneName, sizeof(mp.phoneName), "Raemond's iPhone");
        clearWhite(fb.data());
        ui_render_menu(mp, fb.data());
        ui::statusBar(s, fb.data(), "MENU");
        emit("menu_paired.png");

        mp.phonePaired = false;
        clearWhite(fb.data());
        ui_render_menu(mp, fb.data());
        ui::statusBar(s, fb.data(), "MENU");
        emit("menu_unpaired.png");
    }

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
    ui::statusBar(s, fb.data(), "GPS DEBUG");
    emit("gpsdebug.png");

    // Directions list: every row leads with a drawn maneuver arrow.
    {
        ListRow dirs[4] = {};
        snprintf(dirs[0].title, sizeof(dirs[0].title), "Turn left onto Valencia");
        snprintf(dirs[0].subtitle, sizeof(dirs[0].subtitle), "in 240 m");
        snprintf(dirs[1].title, sizeof(dirs[1].title), "Turn right onto 24th");
        snprintf(dirs[1].subtitle, sizeof(dirs[1].subtitle), "in 1.2 km");
        snprintf(dirs[2].title, sizeof(dirs[2].title), "Continue straight");
        snprintf(dirs[2].subtitle, sizeof(dirs[2].subtitle), "for 3.4 km");
        snprintf(dirs[3].title, sizeof(dirs[3].title), "Make a U-turn");
        snprintf(dirs[3].subtitle, sizeof(dirs[3].subtitle), "in 80 m");
        clearWhite(fb.data());
        ui_render_list("DIRECTIONS", dirs, 4, "4 turns", fb.data(), true);
        ui::statusBar(s, fb.data(), "DIRECTIONS");
        emit("directions.png");
    }

    // Power sheet drawn over the dashboard (overlay behavior)
    clearWhite(fb.data());
    ui_render_dashboard(s, false, dashDefaultLayout(), fb.data());
    ui_render_power_sheet(true, fb.data());
    emit("power.png");

    // Unavailable sensors vanish and the rest expand: the SAME layout with and
    // without a power meter and strap. This is the behaviour to eyeball — a
    // configured field with nothing behind it must not leave a hole, and must
    // not quietly become a different field either.
    {
        RideState noSensors = s;
        noSensors.powerConnected = false;
        noSensors.cadenceConnected = false;
        noSensors.hrConnected = false;
        noSensors.power3sW = 0xFFFF;
        noSensors.heartRateBpm = 0xFF;
        noSensors.cadenceRpm = 0xFF;
        clearWhite(fb.data());
        ui_render_dashboard(noSensors, false, dashDefaultLayout(), fb.data());
        emit("dashboard_no_sensors.png");
    }

    // Show-offline ON with the power meter gone: the HERO field must grey out
    // like any other cell. It did not — the hero path had no stale handling at
    // all, so the biggest number on the panel kept looking live after the meter
    // dropped out.
    {
        RideState heroOff = s;
        heroOff.showOffline = true;
        heroOff.powerConnected = false;
        heroOff.power3sW = 0xFFFF;
        heroOff.powerW = 0xFFFF;
        clearWhite(fb.data());
        ui_render_dashboard(heroOff, false, dashDefaultLayout(), fb.data());
        emit("dashboard_offline_hero.png");
    }

    // Configurable dashboard: a layout NOT expressible before, exercising the
    // packer's awkward cases — a non-power hero, an odd number of half fields
    // (the last one spans), and a small row. Parsed from the config text rather
    // than built struct-by-struct so the file format is covered too.
    {
        const char* cfg =
            "speed     large\n"      // full width, plenty of room
            "distance  large  half\n"
            "ridetime  large  half\n" // long value in a narrow box
            "grade     large  half\n" // no data -> dash on tone
            "altitude  large  half\n";
        DashLayout custom;
        if (dashParse(cfg, custom)) {
            RideState st = s;
            st.gradeValid = false;          // GRADE reads "--"
            st.mapElevationValid = false;   // ALTITUDE reads "--"
            st.speedKmh = 0.0f;
            st.distanceM = 0;
            st.elapsedS = 0;
            clearWhite(fb.data());
            ui_render_dashboard(st, false, custom, fb.data());
            emit("dashboard_custom.png");
        }
    }

    // MUSIC page — playing, with synthetic album art (radial gradient + rings,
    // dithered here exactly as media.cpp would have) and a title long enough
    // to exercise the truncation.
    {
        const int AW = 300, AH = 300;
        static uint8_t art[AW * AH];
        for (int y = 0; y < AH; ++y)
            for (int x = 0; x < AW; ++x) {
                double dx = x - AW / 2, dy = y - AH / 2;
                double d = std::sqrt(dx * dx + dy * dy);
                int v = (int)(255 - d * 1.4) + ((((int)d / 24) & 1) ? 40 : 0);
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                // Same palette the device dithers to; ordered threshold is
                // close enough for a preview.
                static const uint8_t tone[5] = {0x00, 0x11, 0x22, 0x33, 0xFF};
                art[y * AW + x] = tone[v / 52 > 4 ? 4 : v / 52];
            }
        MediaState m;
        m.present = true;
        m.playing = true;
        m.posSec = 154;
        m.durSec = 331;
        snprintf(m.title, sizeof(m.title),
                 "Turn Your Lights Down Low (Extended Remaster)");
        snprintf(m.artist, sizeof(m.artist), "Bob Marley & The Wailers");
        snprintf(m.album, sizeof(m.album), "Exodus");
        m.art = art;
        m.artW = AW;
        m.artH = AH;
        clearWhite(fb.data());
        ui_render_media(s, m, fb.data());
        emit("music_playing.png");

        // Paused, no art yet — the vinyl placeholder.
        m.art = nullptr;
        m.artW = m.artH = 0;
        m.playing = false;
        clearWhite(fb.data());
        ui_render_media(s, m, fb.data());
        emit("music_no_art.png");

        // Nothing playing at all.
        MediaState none;
        clearWhite(fb.data());
        ui_render_media(s, none, fb.data());
        emit("music_idle.png");
    }


    // STOPWATCH page: fresh, running, paused, past the hour (where the
    // hours column steps the numerals down once), and with laps — more than
    // the list shows, running and paused — each with its average speed and,
    // when a source is there, cadence and power. Samples are fed the way the
    // UI task feeds them: a reading held until the next one. The metric
    // columns follow the sensors, so variants cover speed only, speed +
    // cadence, and all three.
    {
        const uint32_t t0 = 1000000;   // arbitrary millis() at START
        // Ride `ms` at a steady speed / power / cadence from `t` (-1 = no
        // source for that metric); returns the end time.
        auto ride = [](Stopwatch& sw, uint32_t t, uint32_t ms, float kmh,
                       int watts, int rpm) {
            for (uint32_t at : {t, t + ms})
                sw.sample(at, kmh, true, watts < 0 ? 0 : (uint16_t)watts,
                          watts >= 0, rpm < 0 ? 0 : (uint8_t)rpm, rpm >= 0);
            return t + ms;
        };

        Stopwatch sw;
        clearWhite(fb.data());
        ui_render_stopwatch(s, sw, t0, fb.data());
        emit("stopwatch_ready.png");

        sw.toggle(t0);
        const uint32_t at1234 = ride(sw, t0, (12 * 60 + 34) * 1000 + 560,
                                     28.6f, 212, 88);
        clearWhite(fb.data());
        ui_render_stopwatch(s, sw, at1234, fb.data());
        emit("stopwatch_running.png");

        Stopwatch paused = sw;
        paused.toggle(at1234);
        clearWhite(fb.data());
        ui_render_stopwatch(s, paused, at1234 + 5000, fb.data());
        emit("stopwatch_paused.png");

        const uint32_t atHour = ride(sw, at1234,
                                     (1 * 3600 + 2 * 60 + 3) * 1000 -
                                         (at1234 - t0),
                                     26.0f, 195, 86);
        clearWhite(fb.data());
        ui_render_stopwatch(s, sw, atHour, fb.data());
        emit("stopwatch_hour.png");

        // No power meter, cadence sensor only: speed + cadence.
        Stopwatch np;
        np.toggle(t0);
        const uint32_t npEnd = ride(np, t0, (8 * 60 + 12) * 1000, 24.3f, -1, 84);
        clearWhite(fb.data());
        ui_render_stopwatch(s, np, npEnd, fb.data());
        emit("stopwatch_running_nopower.png");

        // Speed only (no meter, no cadence sensor).
        Stopwatch so;
        so.toggle(t0);
        const uint32_t soEnd = ride(so, t0, (8 * 60 + 12) * 1000, 24.3f, -1, -1);
        clearWhite(fb.data());
        ui_render_stopwatch(s, so, soEnd, fb.data());
        emit("stopwatch_running_speed.png");

        // Nine hill repeats: the list shows the newest seven. Rendered three
        // times with the sensors a rider might have: all, cadence only, none.
        const uint32_t splits[] = {312400, 298700, 305100, 291300, 322800,
                                   301900, 296200, 288600, 307500};
        const float kmh[] = {14.2f, 14.9f, 14.6f, 15.3f, 13.8f,
                             14.7f, 15.0f, 15.4f, 14.5f};
        const int watts[] = {268, 281, 274, 290, 259, 276, 284, 293, 272};
        const int rpm[] = {78, 81, 79, 83, 74, 80, 82, 84, 79};
        auto repeats = [&](Stopwatch& l, bool power, bool cadence) {
            l.toggle(t0);
            uint32_t t = t0;
            for (int i = 0; i < 9; ++i) {
                t = ride(l, t, splits[i], kmh[i], power ? watts[i] : -1,
                         cadence ? rpm[i] : -1);
                l.lap(t);
            }
            return ride(l, t, 83400, 15.1f, power ? 286 : -1,
                        cadence ? 83 : -1);
        };
        Stopwatch laps;
        uint32_t t = repeats(laps, true, true);
        clearWhite(fb.data());
        ui_render_stopwatch(s, laps, t, fb.data());
        emit("stopwatch_laps.png");

        laps.toggle(t);
        clearWhite(fb.data());
        ui_render_stopwatch(s, laps, t + 6600, fb.data());
        emit("stopwatch_laps_paused.png");

        Stopwatch lapsCad;
        t = repeats(lapsCad, false, true);
        clearWhite(fb.data());
        ui_render_stopwatch(s, lapsCad, t, fb.data());
        emit("stopwatch_laps_cadence.png");

        Stopwatch lapsSpeed;
        t = repeats(lapsSpeed, false, false);
        clearWhite(fb.data());
        ui_render_stopwatch(s, lapsSpeed, t, fb.data());
        emit("stopwatch_laps_speed.png");

        // In miles, and the power meter gone after the first lap: its column
        // stays (lap 1 has power) and the later laps read "--", not 0.
        RideState mi = s;
        mi.useMiles = true;
        Stopwatch two;
        two.toggle(t0);
        uint32_t u = ride(two, t0, 95300, 32.4f, 245, 92);
        two.lap(u);
        u = ride(two, u, 101800, 30.9f, -1, 90);
        two.lap(u);
        u = ride(two, u, 41000, 33.8f, -1, 94);
        clearWhite(fb.data());
        ui_render_stopwatch(mi, two, u, fb.data());
        emit("stopwatch_laps_two.png");

        // Past the hour with laps: H:MM:SS has to fit beside the TOTAL label
        // and in the SPLIT column with all five columns up.
        Stopwatch longer;
        longer.toggle(t0);
        uint32_t v = ride(longer, t0, 3700000, 27.3f, 201, 87);
        longer.lap(v);
        v = ride(longer, v, 1834000, 25.8f, 188, 85);
        clearWhite(fb.data());
        ui_render_stopwatch(s, longer, v, fb.data());
        emit("stopwatch_laps_hour.png");
    }

    // SEGMENT page (live Strava segments): idle with the nearest start, no
    // segments, none nearby, at the start, mid-effort ahead / behind / off
    // the line, and the finish (new PR, slower, first time) and an abandon.
    // The map and profile come from a real parsed set: a 2.4 km switchback
    // climb (5.7 %) and a 5.4 km road climb, encoded as the app would send
    // them (v2, with elevation), plus a v1 copy with none. The views are built
    // directly — the matcher behind them is covered by tools/segment_test.
    {
        // Hawk Hill: east, then three switchbacks up the hill, 5.7 % average.
        const double lat0 = 37.8270, lon0 = -122.4990;
        const double mLon = 111320.0 * cos(lat0 * M_PI / 180.0);
        std::vector<int32_t> la, lo;
        std::vector<float> el;
        std::vector<float> cumM;
        auto add = [&](double e, double n) {
            if (!la.empty()) {
                const double pe = (lo.back() / 1e5 - lon0) * mLon;
                const double pn = (la.back() / 1e5 - lat0) * 110540.0;
                cumM.push_back(cumM.back() + (float)sqrt((e - pe) * (e - pe) + (n - pn) * (n - pn)));
            } else {
                cumM.push_back(0);
            }
            la.push_back((int32_t)lround((lat0 + n / 110540.0) * 1e5));
            lo.push_back((int32_t)lround((lon0 + e / mLon) * 1e5));
        };
        const double pts[][2] = {{0, 0},     {120, 30},  {260, 40},  {380, 110},
                                 {420, 230}, {340, 330}, {200, 360}, {90, 420},
                                 {60, 530},  {150, 610}, {300, 640}, {450, 660},
                                 {560, 730}, {580, 850}, {500, 950}, {380, 1010},
                                 {300, 1100}};
        for (auto& p : pts) {
            // Densify every ~25 m, as the app's cut keeps a road's bends.
            if (!la.empty()) {
                const double pe = (lo.back() / 1e5 - lon0) * mLon;
                const double pn = (la.back() / 1e5 - lat0) * 110540.0;
                const int n = (int)(sqrt((p[0] - pe) * (p[0] - pe) + (p[1] - pn) * (p[1] - pn)) / 25);
                for (int i = 1; i < n; ++i)
                    add(pe + (p[0] - pe) * i / n, pn + (p[1] - pn) * i / n);
            }
            add(p[0], p[1]);
        }
        const float total = cumM.back();
        for (float c : cumM) {
            const float f = c / total;
            // Steeper in the middle, a flat bit two thirds up, a kick at the end.
            el.push_back(68 + 120 * f + 18 * sinf(f * 6.3f) - (f > 0.62f && f < 0.72f ? 8 * (f - 0.62f) / 0.1f : 0) +
                         (f > 0.9f ? 12 * (f - 0.9f) / 0.1f : 0));
        }
        // A second, longer road climb for the long-name frame.
        std::vector<int32_t> la2, lo2;
        std::vector<float> el2;
        for (int i = 0; i <= 120; ++i) {
            const double f = i / 120.0;
            la2.push_back((int32_t)lround((37.3800 + 0.030 * f + 0.004 * sin(f * 18)) * 1e5));
            lo2.push_back((int32_t)lround((-122.2500 - 0.020 * f + 0.003 * cos(f * 13)) * 1e5));
            el2.push_back((float)(110 + 395 * f + 15 * sin(f * 9)));
        }
        seg::SourceSegment src[2] = {
            {229781, "Hawk Hill Climb", 524, 24100, 57, (int)la.size(), la.data(), lo.data(), el.data()},
            {8109834, "Old La Honda (Bridge to Mailboxes) official", 1984, 54200, 73,
             (int)la2.size(), la2.data(), lo2.data(), el2.data()},
        };
        static uint8_t blob[seg::BLOB_MAX];
        static seg::SegmentDb db, dbV1;
        seg::parseBlob(blob, seg::encodeBlob(src, 2, blob, sizeof(blob)), db);
        seg::parseBlob(blob, seg::encodeBlob(src, 2, blob, sizeof(blob), 1), dbV1);
        const seg::Segment& hawk = db.segs[0];
        // Where a rider `along` metres in is, in the segment's frame.
        auto riderAt = [&](seg::View& v, const seg::SegmentDb& d, int idx, float along) {
            const seg::Segment& sg = d.segs[idx];
            const float* C = d.cum + sg.first;
            int i = 0;
            while (i + 2 < sg.nPts && C[i + 1] < along) ++i;
            const float t = fminf(fmaxf((along - C[i]) / fmaxf(C[i + 1] - C[i], 1e-3f), 0), 1);
            v.riderValid = true;
            v.riderX = d.x[sg.first + i] + t * (d.x[sg.first + i + 1] - d.x[sg.first + i]);
            v.riderY = d.y[sg.first + i] + t * (d.y[sg.first + i + 1] - d.y[sg.first + i]);
        };

        auto page = [&](const seg::View& v, const char* name,
                        const RideState& rs) {
            clearWhite(fb.data());
            ui_render_segment(rs, v, fb.data());
            emit(name);
        };
        auto named = [&](seg::View& v, int idx, uint32_t pr) {
            const seg::Segment& sg = db.segs[idx];
            snprintf(v.name, sizeof(v.name), "%s", sg.name);
            v.lenM = sg.lenM;
            v.gradeD = sg.gradeD;
            v.prSec = pr;
            v.db = &db;
            v.segIndex = idx;
        };
        seg::View idle;
        idle.count = 23;
        idle.haveNearest = true;
        snprintf(idle.nearestName, sizeof(idle.nearestName), "%s", hawk.name);
        idle.nearestM = 1240;
        idle.nearestLenM = hawk.lenM;
        idle.nearestGradeD = 57;
        idle.nearestPrSec = 524;
        idle.nearestBearingDeg = 40;
        idle.db = &db;
        idle.segIndex = 0;
        page(idle, "segment_idle.png", s);
        RideState mi = s;
        mi.useMiles = true;
        seg::View close = idle;
        close.nearestM = 180;
        close.riderValid = true;   // 180 m short of the start, on the map
        close.riderX = -150;
        close.riderY = -60;
        page(close, "segment_idle_close_miles.png", mi);
        // A set synced by a v1 app: the line, no profile.
        seg::View old = idle;
        old.db = &dbV1;
        page(old, "segment_idle_no_elevation.png", s);

        seg::View none;
        page(none, "segment_none.png", s);
        seg::View far;
        far.count = 23;
        page(far, "segment_far.png", s);

        seg::View armed;
        armed.count = 23;
        armed.phase = seg::ARMED;
        named(armed, 0, 524);
        armed.riderValid = true;
        armed.riderX = -8;
        armed.riderY = -4;
        page(armed, "segment_armed.png", s);

        seg::View act = armed;
        act.phase = seg::ACTIVE;
        act.doneM = hawk.lenM * 0.61f;
        riderAt(act, db, 0, act.doneM);
        act.elapsedMs = 309400;
        act.haveDelta = true;
        act.deltaMs = -11300;
        page(act, "segment_active_ahead.png", s);
        // No sensors: the POWER / HEART RATE row goes, the map takes it.
        RideState bare = s;
        bare.powerConnected = bare.hrConnected = false;
        page(act, "segment_active_nosensors.png", bare);
        // A power meter alone: its tile spans the row.
        RideState pwrOnly = s;
        pwrOnly.hrConnected = false;
        act.deltaMs = 14800;
        page(act, "segment_active_power_only.png", pwrOnly);
        page(act, "segment_active_behind.png", s);
        seg::View noPr = act;
        noPr.haveDelta = false;
        noPr.prSec = 0;
        page(noPr, "segment_active_no_pr.png", s);
        act.offRoute = true;
        act.riderX += 70;
        page(act, "segment_active_off.png", s);
        seg::View actMi = act;
        actMi.offRoute = false;
        riderAt(actMi, db, 0, actMi.doneM);
        page(actMi, "segment_active_miles.png", mi);

        seg::View longName = armed;
        longName.phase = seg::ACTIVE;
        named(longName, 1, 1984);
        longName.doneM = 610;
        riderAt(longName, db, 1, 610);
        longName.elapsedMs = 241000;
        longName.haveDelta = true;
        longName.deltaMs = 18000;
        page(longName, "segment_active_long_name.png", s);

        // Several relevant segments: the < n/N > pager in the status bar.
        // Two overlapping efforts, the first and then the second shown...
        seg::View ov1 = act;
        ov1.offRoute = false;
        riderAt(ov1, db, 0, ov1.doneM);
        ov1.deltaMs = -11300;
        ov1.listPos = 1;
        ov1.listCount = 2;
        page(ov1, "segment_multi_overlap_1of2.png", s);
        seg::View ov2 = longName;
        ov2.listPos = 2;
        ov2.listCount = 2;
        page(ov2, "segment_multi_overlap_2of2.png", s);
        // ...and one effort with a start coming up: the effort first, the
        // approaching segment as page 2.
        seg::View ap1 = ov1;
        page(ap1, "segment_multi_active_approaching_1of2.png", s);
        seg::View ap2;
        ap2.count = 23;
        ap2.haveNearest = true;
        snprintf(ap2.nearestName, sizeof(ap2.nearestName), "%s", db.segs[1].name);
        ap2.nearestM = 640;
        ap2.nearestLenM = db.segs[1].lenM;
        ap2.nearestGradeD = db.segs[1].gradeD;
        ap2.nearestPrSec = 1984;
        ap2.nearestBearingDeg = 300;
        ap2.db = &db;
        ap2.segIndex = 1;
        ap2.listPos = 2;
        ap2.listCount = 2;
        page(ap2, "segment_multi_active_approaching_2of2.png", s);

        seg::View done = armed;
        done.phase = seg::DONE;
        done.doneM = hawk.lenM;
        riderAt(done, db, 0, hawk.lenM);
        done.elapsedMs = 509000;
        done.haveDelta = true;
        done.deltaMs = -15000;
        done.newPr = true;
        done.avgPowerW = 268;
        done.avgHrBpm = 156;
        page(done, "segment_done_pr.png", s);
        seg::View doneBare = done;
        doneBare.avgPowerW = 0xFFFF;
        doneBare.avgHrBpm = 0;
        page(doneBare, "segment_done_pr_nosensors.png", bare);
        done.deltaMs = 22000;
        done.newPr = false;
        page(done, "segment_done_slower.png", s);
        seg::View first = done;
        first.haveDelta = false;
        first.newPr = true;
        first.prSec = 0;
        page(first, "segment_done_first.png", s);

        seg::View gone = armed;
        gone.phase = seg::ABANDONED;
        gone.doneM = 880;
        riderAt(gone, db, 0, 880);
        gone.riderX += 160;
        gone.elapsedMs = 201000;
        page(gone, "segment_abandoned.png", s);
    }

    // WORKOUT pages, per the Claude Design "Workout View" frames: executing
    // a block (in range and under target) and the ALL BLOCKS list.
    {
        static const char* kErg =
            "[COURSE HEADER]\n"
            "MINUTES WATTS\n"
            "[END COURSE HEADER]\n"
            "[COURSE DATA]\n"
            "0 120\n10 180\n"
            "10 260\n15 260\n15 140\n18 140\n"
            "18 260\n23 260\n23 140\n26 140\n"
            "26 260\n31 260\n31 140\n34 140\n"
            "34 260\n39 260\n"
            "39 120\n45 100\n"
            "[END COURSE DATA]\n";
        static Workout wk;
        workoutParse(kErg, 250, wk);
        snprintf(wk.name, sizeof(wk.name), "4X5 THRESHOLD");

        RideState ws = s;
        ws.powerConnected = true;
        WorkoutView v;

        ws.power3sW = 264;   // inside the band
        workoutBuildView(wk, 20 * 60, true, 250, v);
        clearWhite(fb.data());
        ui_render_workout(ws, v, fb.data());
        emit("workout_on_target.png");

        ws.power3sW = 205;   // 55 W under — GO HARDER
        workoutBuildView(wk, 27 * 60, true, 250, v);
        clearWhite(fb.data());
        ui_render_workout(ws, v, fb.data());
        emit("workout_add.png");

        // Paused mid-block, no meter data.
        ws.power3sW = 0xFFFF;
        ws.powerConnected = false;
        workoutBuildView(wk, 21 * 60, false, 250, v);
        v.paused = true;
        clearWhite(fb.data());
        ui_render_workout(ws, v, fb.data());
        emit("workout_paused.png");

        // The ALL BLOCKS list, both pages.
        ws.power3sW = 264;
        ws.powerConnected = true;
        workoutBuildView(wk, 27 * 60, true, 250, v);
        clearWhite(fb.data());
        ui_render_workout_list(ws, v, 0, fb.data());
        emit("workout_list.png");
        clearWhite(fb.data());
        ui_render_workout_list(ws, v, 1, fb.data());
        emit("workout_list_p2.png");

        WorkoutView none;
        clearWhite(fb.data());
        ui_render_workout(ws, none, fb.data());
        emit("workout_none.png");

        // READY (loaded, not started): the left strip is the way back to the
        // picker instead of "redo".
        ws.power3sW = 0xFFFF;
        ws.powerConnected = false;
        workoutBuildView(wk, 0, false, 250, v);
        clearWhite(fb.data());
        ui_render_workout(ws, v, fb.data());
        emit("workout_ready_back.png");
    }

    // The device-side PICKER: the WORKOUT page with nothing loaded lists the
    // files in /workouts. Built here exactly as workout_service builds its
    // cache: parse, title from the header, summarize.
    {
        struct Blk { double min; int pct; };
        struct Lib {
            const char* file;
            const char* desc;   // nullptr: no DESCRIPTION line
            std::vector<Blk> blocks;
        };
        auto rep = [](std::vector<Blk> base, int n, std::vector<Blk> on,
                      std::vector<Blk> tail) {
            for (int i = 0; i < n; ++i)
                base.insert(base.end(), on.begin(), on.end());
            base.insert(base.end(), tail.begin(), tail.end());
            return base;
        };
        const std::vector<Lib> lib = {
            {"sweetspot_3x15.mrc", "Sweet Spot 3x15",
             rep({{10, 55}}, 3, {{15, 90}, {5, 50}}, {{5, 45}})},
            {"4x5_threshold.mrc", "4x5 Threshold",
             rep({{10, 60}}, 4, {{5, 104}, {3, 55}}, {{6, 45}})},
            {"vo2_5x3.mrc", "VO2 Max 5x3",
             rep({{12, 55}}, 5, {{3, 120}, {3, 50}}, {{8, 45}})},
            {"recovery.mrc", "Recovery Spin", {{40, 50}}},
            {"over_unders.mrc", "Over-Unders 3x12",
             rep({{10, 55}}, 3,
                 {{2, 95}, {1, 108}, {2, 95}, {1, 108}, {2, 95}, {1, 108},
                  {2, 95}, {1, 108}, {5, 50}},
                 {{5, 45}})},
            {"hill_repeats.erg", nullptr,   // title from the filename
             rep({{10, 55}}, 6, {{2, 115}, {4, 50}}, {{6, 45}})},
            {"endurance_90.mrc", "Endurance 90",
             {{10, 55}, {70, 68}, {10, 50}}},
            {"tempo_2x20.mrc", "Tempo 2x20",
             {{10, 55}, {20, 82}, {5, 55}, {20, 82}, {5, 45}}},
            {"microbursts.mrc", "Microbursts 30/30",
             rep({{10, 55}}, 12, {{0.5, 150}, {0.5, 50}}, {{8, 45}})},
            {"pyramid.mrc", "Pyramid",
             {{8, 55}, {3, 80}, {3, 90}, {3, 100}, {3, 110}, {3, 120},
              {3, 110}, {3, 100}, {3, 90}, {3, 80}, {8, 50}}},
            {"ramp_test.mrc", "Ramp Test",
             rep({{5, 50}}, 1,
                 {{1, 60}, {1, 70}, {1, 80}, {1, 90}, {1, 100}, {1, 110},
                  {1, 120}, {1, 130}, {1, 140}},
                 {{4, 40}})},
            {"sprint_openers.mrc", "Sprint Openers",
             rep({{15, 60}}, 4, {{0.25, 200}, {3, 50}}, {{5, 45}})},
            {"long_z2.mrc", "Zone 2 Long Ride",
             {{10, 55}, {150, 66}, {10, 50}}},
            {"broken.erg", "Half-written file", {}},
        };
        static WorkoutFileInfo all[16];
        static Workout scratch;
        int nAll = 0;
        for (const Lib& l : lib) {
            std::string t = "[COURSE HEADER]\nVERSION = 2\nUNITS = ENGLISH\n";
            if (l.desc) t += std::string("DESCRIPTION = ") + l.desc + "\n";
            t += std::string("FILE NAME = ") + l.file + "\n";
            t += "MINUTES PERCENT\n[END COURSE HEADER]\n[COURSE DATA]\n";
            double m = 0;
            char line[48];
            for (const Blk& b : l.blocks) {
                snprintf(line, sizeof(line), "%.2f %d\n", m, b.pct);
                t += line;
                m += b.min;
                snprintf(line, sizeof(line), "%.2f %d\n", m, b.pct);
                t += line;
            }
            t += "[END COURSE DATA]\n";
            WorkoutFileInfo& e = all[nAll++];
            snprintf(e.file, sizeof(e.file), "%s", l.file);
            workoutTitleFrom(t.c_str(), l.file, e.title, sizeof(e.title));
            if (workoutParse(t.c_str(), 250, scratch))
                workoutSummarize(scratch, 250, e);
        }
        std::sort(all, all + nAll,
                  [](const WorkoutFileInfo& a, const WorkoutFileInfo& b) {
                      return strcasecmp(a.title, b.title) < 0;
                  });
        // The same page cut workout_service::listPage makes.
        auto page = [&](const WorkoutFileInfo* src, int n, int pg,
                        WorkoutPickPage& p) {
            p = WorkoutPickPage{};
            p.state = WLIST_READY;
            p.total = n;
            p.first = pg * WORKOUT_PICK_ROWS;
            for (int i = p.first; i < n && p.count < WORKOUT_PICK_ROWS; ++i)
                p.rows[p.count++] = src[i];
        };

        RideState ps = s;
        static WorkoutPickPage p;

        // Empty card.
        page(all, 0, 0, p);
        clearWhite(fb.data());
        ui_render_workout_picker(ps, p, fb.data());
        emit("workout_picker_empty.png");

        // A few workouts, one of them mid-load after a tap.
        static WorkoutFileInfo few[4];
        const char* pick[] = {"4x5_threshold.mrc", "recovery.mrc",
                              "sweetspot_3x15.mrc", "vo2_5x3.mrc"};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < nAll; ++j)
                if (!strcmp(all[j].file, pick[i])) few[i] = all[j];
        page(few, 4, 0, p);
        clearWhite(fb.data());
        ui_render_workout_picker(ps, p, fb.data());
        emit("workout_picker_few.png");
        snprintf(p.loading, sizeof(p.loading), "sweetspot_3x15.mrc");
        clearWhite(fb.data());
        ui_render_workout_picker(ps, p, fb.data());
        emit("workout_picker_loading.png");

        // Many: two pages, with a file that wouldn't parse, and the header
        // reporting a load that failed.
        page(all, nAll, 0, p);
        clearWhite(fb.data());
        ui_render_workout_picker(ps, p, fb.data());
        emit("workout_picker_many_p1.png");
        page(all, nAll, 1, p);
        snprintf(p.error, sizeof(p.error), "broken.erg: no course data parsed");
        clearWhite(fb.data());
        ui_render_workout_picker(ps, p, fb.data());
        emit("workout_picker_many_p2.png");

        // Before the first scan lands, and with no card.
        p = WorkoutPickPage{};
        clearWhite(fb.data());
        ui_render_workout_picker(ps, p, fb.data());
        emit("workout_picker_scanning.png");
        p.state = WLIST_NO_CARD;
        clearWhite(fb.data());
        ui_render_workout_picker(ps, p, fb.data());
        emit("workout_picker_no_card.png");
    }

    // BLE pairing sheet over the dashboard
    clearWhite(fb.data());
    ui_render_dashboard(s, false, dashDefaultLayout(), fb.data());
    ui_render_pairing(831427, fb.data());
    emit("pairing.png");

    // Nav prompt over the map

    // Turn-by-turn banner over the map. navBannerVisible must move the compass
    // clear of the banner — it used to be drawn underneath it and invisible.
    clearWhite(fb.data());
    map.navBannerVisible = true;
    ui_render_map(map, s, fb.data());
    ui_render_nav_banner("Turn left onto Valencia St", 180, false, fb.data());
    emit("nav_banner.png");
    map.navBannerVisible = false;

    // The pairing sheet over the map page mid-ride: the code must show on
    // every screen, recording or not.
    {
        RideState rs = s;
        rs.recording = true;
        clearWhite(fb.data());
        ui_render_map(map, rs, fb.data());
        ui_render_pairing(831427, fb.data());
        emit("pairing_map_recording.png");
    }

    // LAST on purpose. `map` holds POINTERS into map_tiles' projected buffers
    // and the route preview re-projects them for its own viewport, so any map
    // screen rendered after this one would silently inherit the preview's
    // framing rather than its own.
    // The accept page, composed exactly as ui_dashboard.cpp composes it: the
    // WHOLE route fitted so it can be recognized before accepting, then the
    // status band cleared and redrawn over it (the map renderer does not clip,
    // so its road and water fills would otherwise paint over the clock), then
    // the sheet. Rendering it over the live map instead — which is what this
    // did — showed a route preview containing no route.
    //
    // Alamo Square down to Mission Dolores, the route the sheet names.
    ::routes::gRoute = {
        {37.7764, -122.4346}, {37.7742, -122.4344}, {37.7712, -122.4341},
        {37.7690, -122.4297}, {37.7668, -122.4293}, {37.7641, -122.4288},
        {37.7618, -122.4275}, {37.7596, -122.4269},
    };
    clearWhite(fb.data());
    ui_render_route_preview(fb.data());
    epd_fill_rect({0, 0, epd_rotated_display_width(), ui::STATUS_H}, 0xFF,
                  fb.data());
    ui::statusBar(s, fb.data());
    ui_render_nav_prompt(::routes::activeName(), 8, fb.data());
    emit("nav_prompt.png");
    // ...and over the nav prompt, which ui_dashboard draws outside the
    // normal-screens branch (the sheet used to be skipped there).
    ui_render_pairing(831427, fb.data());
    emit("pairing_nav_prompt.png");
    ::routes::gRoute.clear();

    return 0;
}
