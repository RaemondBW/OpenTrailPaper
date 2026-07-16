#include "map_view.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "epdiy.h"
#include "ride_state.h"
#include "routes.h"
#include "map_store.h"
#include "ui_render.h"
#include "fonts/arialbold_14.h"
#include "fonts/arialbold_20.h"
#include "fonts/impact_40.h"

namespace {

// Layout bands (portrait 540x960, design 1f)
constexpr int MAP_TOP = ui::STATUS_H;
constexpr int STRIP_TOP = 810;   // 3-cell footer below
constexpr int MAP_BOTTOM = STRIP_TOP;

// The fast DU refresh is strictly 1-bit (grays snap to white), so all
// map ink must be pure black. Road classes differ by width/dash instead.
constexpr uint8_t ROAD_INK = 0x00;
constexpr uint8_t WATER_GRAY = 0xC8;  // rare; invisible in DU, fine

struct Style {
    int width;
    uint8_t color;
    int dashLen;     // 0 = solid
    int gapLen;
};

Style styleFor(MapFeatureClass cls) {
    switch (cls) {
        case MAP_ROAD_MAJOR: return {5, ROAD_INK, 0, 0};
        case MAP_ROAD_MINOR: return {2, ROAD_INK, 0, 0};
        case MAP_PATH:       return {2, ROAD_INK, 8, 8};
        case MAP_RAIL:       return {2, ROAD_INK, 14, 12};
        case MAP_WATER:      return {18, WATER_GRAY, 0, 0};
    }
    return {1, 0x00, 0, 0};
}

// Thick segment as a quad (two triangles) plus round caps.
void thickSegment(float x0, float y0, float x1, float y1, int width,
                  uint8_t color, uint8_t* fb) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.5f) return;
    float half = width / 2.0f;
    float px = -dy / len * half, py = dx / len * half;

    int ax = lroundf(x0 + px), ay = lroundf(y0 + py);
    int bx = lroundf(x0 - px), by = lroundf(y0 - py);
    int cx = lroundf(x1 - px), cy = lroundf(y1 - py);
    int ex = lroundf(x1 + px), ey = lroundf(y1 + py);
    epd_fill_triangle(ax, ay, bx, by, cx, cy, color, fb);
    epd_fill_triangle(ax, ay, cx, cy, ex, ey, color, fb);

    if (width >= 4) {
        epd_fill_circle(lroundf(x0), lroundf(y0), (int)half, color, fb);
        epd_fill_circle(lroundf(x1), lroundf(y1), (int)half, color, fb);
    }
}

void drawSegmentStyled(float x0, float y0, float x1, float y1,
                       const Style& st, uint8_t* fb) {
    if (st.dashLen == 0) {
        thickSegment(x0, y0, x1, y1, st.width, st.color, fb);
        return;
    }
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.5f) return;
    float ux = dx / len, uy = dy / len;
    float pos = 0;
    while (pos < len) {
        float end = pos + st.dashLen;
        if (end > len) end = len;
        thickSegment(x0 + ux * pos, y0 + uy * pos, x0 + ux * end,
                     y0 + uy * end, st.width, st.color, fb);
        pos = end + st.gapLen;
    }
}

void drawPolyline(const int16_t* pts, int count, const Style& st, uint8_t* fb) {
    for (int i = 0; i + 1 < count; ++i) {
        drawSegmentStyled(pts[i * 2], pts[i * 2 + 1], pts[i * 2 + 2],
                          pts[i * 2 + 3], st, fb);
    }
}

void drawRider(int x, int y, float headingDeg, uint8_t* fb) {
    float rad = (headingDeg - 90.0f) * (float)M_PI / 180.0f;
    float c = cosf(rad), s = sinf(rad);
    auto rot = [&](float lx, float ly, int& ox, int& oy) {
        ox = x + lroundf(lx * c - ly * s);
        oy = y + lroundf(lx * s + ly * c);
    };
    int nx, ny, lx, ly, rx, ry, bx, by;
    rot(24, 0, nx, ny);
    rot(-15, -13, lx, ly);
    rot(-15, 13, rx, ry);
    rot(-8, 0, bx, by);

    // White halo circle with black ring (design: circled position marker)
    epd_fill_circle(x, y, 30, 0xFF, fb);
    for (int r = 28; r <= 30; ++r) epd_draw_circle(x, y, r, 0x00, fb);
    epd_fill_triangle(nx, ny, lx, ly, bx, by, 0x00, fb);
    epd_fill_triangle(nx, ny, rx, ry, bx, by, 0x00, fb);
}

void drawScaleBar(float metersPerPixel, bool miles, uint8_t* fb) {
    int x = 20, y = MAP_BOTTOM - 26;
    char buf[16];
    float barM;
    if (miles) {
        // Round imperial rungs (metres, label): feet then miles.
        struct Rung { float m; const char* lbl; };
        static const Rung rungs[] = {
            {30.48f, "100 FT"}, {76.2f, "250 FT"}, {152.4f, "500 FT"},
            {304.8f, "1000 FT"}, {804.67f, "0.5 MI"}, {1609.34f, "1 MI"},
            {3218.69f, "2 MI"}};
        const Rung* pick = &rungs[0];
        for (const Rung& r : rungs) if (r.m / metersPerPixel <= 160) pick = &r;
        barM = pick->m;
        snprintf(buf, sizeof(buf), "%s", pick->lbl);
    } else {
        const int targets[] = {50, 100, 200, 500, 1000, 2000};
        int meters = targets[0];
        for (int t : targets) if (t / metersPerPixel <= 160) meters = t;
        barM = meters;
        if (meters >= 1000) snprintf(buf, sizeof(buf), "%d KM", meters / 1000);
        else snprintf(buf, sizeof(buf), "%d M", meters);
    }
    int px = (int)(barM / metersPerPixel);
    epd_fill_rect({x, y, px, 4}, 0x00, fb);
    ui::text(&ArialBold_20, x, y - 10, buf, fb);
}

void drawCompass(int cx, int cy, float northDeg, bool trackUp, uint8_t* fb) {
    epd_fill_circle(cx, cy, 28, 0xFF, fb);
    for (int r = 26; r <= 28; ++r) epd_draw_circle(cx, cy, r, 0x00, fb);

    // Needle points to true north.
    float a = northDeg * (float)M_PI / 180.0f;
    float nxd = sinf(a), nyd = -cosf(a);   // unit vector toward north
    float pxd = -nyd, pyd = nxd;           // perpendicular
    int tipX = cx + lroundf(nxd * 19), tipY = cy + lroundf(nyd * 19);
    int b1X = cx + lroundf(pxd * 7), b1Y = cy + lroundf(pyd * 7);
    int b2X = cx - lroundf(pxd * 7), b2Y = cy - lroundf(pyd * 7);
    epd_fill_triangle(tipX, tipY, b1X, b1Y, b2X, b2Y, 0x00, fb);

    if (trackUp) {
        // underline marks track-up mode
        epd_fill_rect({cx - 12, cy + 34, 24, 3}, 0x00, fb);
    }
}

}  // namespace

const MapTouchZones kMapZoom = {540 - 78, 560, 640, 76};
const MapCompassZone kMapCompass = {540 - 46, 64 + 48, 34};

void ui_render_map_features(const MapScreenData& map, const RideState& s,
                            uint8_t* fb) {
    (void)s;
    // Map features: water under roads, all in grays
    const MapFeatureClass order[] = {MAP_WATER, MAP_RAIL, MAP_PATH,
                                     MAP_ROAD_MINOR, MAP_ROAD_MAJOR};
    for (MapFeatureClass cls : order) {
        for (int i = 0; i < map.featureCount; ++i) {
            if (map.features[i].cls != cls) continue;
            drawPolyline(map.features[i].pts, map.features[i].pointCount,
                         styleFor(cls), fb);
        }
    }

    // Route: 14 px black; ridden part solid, ahead dashed (design 1f)
    if (map.route && map.routePointCount > 1) {
        int ridden = map.riddenPointCount;
        if (ridden < 1) ridden = 1;
        if (ridden > map.routePointCount) ridden = map.routePointCount;
        drawPolyline(map.route, ridden, {14, 0x00, 0, 0}, fb);
        if (ridden < map.routePointCount) {
            drawPolyline(map.route + (ridden - 1) * 2,
                         map.routePointCount - ridden + 1,
                         {14, 0x00, 26, 16}, fb);
        }
    }

    drawRider(map.riderX, map.riderY, map.headingDeg, fb);
}

void ui_render_map(const MapScreenData& map, const RideState& s, uint8_t* fb) {
    const int W = epd_rotated_display_width();
    const int H = epd_rotated_display_height();
    char buf[32];

    ui_render_map_features(map, s, fb);
    drawScaleBar(map.metersPerPixel, s.useMiles, fb);
    drawCompass(kMapCompass.cx, kMapCompass.cy, map.northDeg, map.trackUp, fb);

    // No map covers this position — tell the rider how to get one instead of
    // showing a blank screen.
    if (!map.hasMap) {
        const int bw = 500, bh = 96;
        const int bx = (W - bw) / 2, by = (H - bh) / 2 - 30;
        epd_fill_rect({bx, by, bw, bh}, 0xFF, fb);
        for (int i = 0; i < 2; ++i)
            epd_draw_rect({bx - i, by - i, bw + 2 * i, bh + 2 * i}, 0x00, fb);
        ui::text(&ArialBold_20, W / 2, by + 38, "NO MAP HERE", fb,
                 EPD_DRAW_ALIGN_CENTER, 0x00);
        ui::text(&ArialBold_14, W / 2, by + 70, "Download this area from the app",
                 fb, EPD_DRAW_ALIGN_CENTER, 0x00);
    }

    // Badge when the map is centered on the phone's position (device GPS cold).
    if (map.phonePosition) {
        EpdRect b = {12, ui::STATUS_H + 8, 150, 28};
        epd_fill_rect(b, 0xFF, fb);
        epd_draw_rect(b, 0x00, fb);
        ui::text(&ArialBold_14, b.x + 9, b.y + 20, "PHONE GPS", fb,
                 EPD_DRAW_ALIGN_LEFT, 0x00);
    }

    // Zoom buttons (design 1f, right edge)
    for (int i = 0; i < 2; ++i) {
        int by = i == 0 ? kMapZoom.zoomInY : kMapZoom.zoomOutY;
        EpdRect r = {kMapZoom.zoomX, by, kMapZoom.size, kMapZoom.size};
        epd_fill_rect(r, 0xFF, fb);
        epd_draw_rect(r, 0x00, fb);
        EpdRect r2 = {r.x + 1, r.y + 1, r.width - 2, r.height - 2};
        epd_draw_rect(r2, 0x00, fb);
        int cx = r.x + r.width / 2, cy = r.y + r.height / 2;
        epd_fill_rect({cx - 14, cy - 2, 28, 5}, 0x00, fb);
        if (i == 0) epd_fill_rect({cx - 2, cy - 14, 5, 28}, 0x00, fb);
    }

    // Status bar drawn after the map (epdiy has no clipping)
    epd_fill_rect({0, 0, W, ui::STATUS_H - 3}, 0xFF, fb);
    ui::statusBar(s, fb);

    // Footer: SPEED | DISTANCE | TIME (design 1f)
    epd_fill_rect({0, STRIP_TOP, W, H - STRIP_TOP}, 0xFF, fb);
    epd_fill_rect({0, STRIP_TOP, W, 3}, 0x00, fb);
    int colW = W / 3;
    for (int i = 1; i < 3; ++i) {
        epd_fill_rect({i * colW - 1, STRIP_TOP + 3, 3, H - STRIP_TOP - 3}, 0x00, fb);
    }

    // Short unit-less headers: a "SPEED KM/H" header is wider than the 180px
    // column and overlaps its neighbours. Units are shown on the dashboard,
    // summary, and (for distance) the scale bar; the map footer stays glanceable.
    ui::label(colW / 2, STRIP_TOP + 44, "SPEED", fb);
    snprintf(buf, sizeof(buf), "%.1f", units::speed(s.speedKmh, s.useMiles));
    ui::valueWithUnit(&Impact_40, 6, colW - 6, H - 40, buf, "", fb);

    ui::label(colW + colW / 2, STRIP_TOP + 44, "DIST", fb);
    snprintf(buf, sizeof(buf), "%.1f", units::distM(s.distanceM, s.useMiles));
    ui::valueWithUnit(&Impact_40, colW + 6, 2 * colW - 6, H - 40, buf, "", fb);

    if (map.showRemaining) {
        ui::label(2 * colW + colW / 2, STRIP_TOP + 44, "LEFT", fb);
        snprintf(buf, sizeof(buf), "%.1f", units::dist(map.remainingKm, s.useMiles));
    } else {
        ui::label(2 * colW + colW / 2, STRIP_TOP + 44, "TIME", fb);
        snprintf(buf, sizeof(buf), "%lu:%02lu", (unsigned long)(s.elapsedS / 3600),
                 (unsigned long)((s.elapsedS / 60) % 60));
    }
    ui::valueWithUnit(&Impact_40, 2 * colW + 6, W - 6, H - 40, buf, "", fb);
}

// Whole-route preview for the "Start navigation?" accept page: fits the entire
// route into the area above the prompt sheet so the rider can recognize it
// before accepting. Independent of the live map zoom/center.
void ui_render_route_preview(uint8_t* fb) {
    const int n = routes::pointCount();
    if (n < 2) return;
    const int W = epd_rotated_display_width();

    // Viewport: below the status bar + route-name band, above the accept sheet.
    const int nameBandH = 40;
    const int top = ui::STATUS_H + nameBandH + 8, bot = 600 - 16;
    const int left = 20, right = W - 20;
    const int vw = right - left, vh = bot - top;

    double minLat = 90, maxLat = -90, minLon = 180, maxLon = -180;
    for (int i = 0; i < n; ++i) {
        double la, lo;
        routes::point(i, la, lo);
        if (la < minLat) minLat = la;
        if (la > maxLat) maxLat = la;
        if (lo < minLon) minLon = lo;
        if (lo > maxLon) maxLon = lo;
    }
    const double clat = (minLat + maxLat) / 2, clon = (minLon + maxLon) / 2;
    const double cosc = cos(clat * M_PI / 180.0);
    double mLat = (maxLat - minLat) * 111320.0;
    double mLon = (maxLon - minLon) * 111320.0 * cosc;
    if (mLat < 1) mLat = 1;
    if (mLon < 1) mLon = 1;
    double mpp = fmax(mLon / vw, mLat / vh) * 1.12;   // fit + 12% margin
    if (mpp < 0.5) mpp = 0.5;

    const int cx = left + vw / 2, cy = top + vh / 2;
    auto toScreen = [&](double la, double lo, int16_t& sx, int16_t& sy) {
        sx = (int16_t)(cx + (lo - clon) * 111320.0 * cosc / mpp);
        sy = (int16_t)(cy - (la - clat) * 111320.0 / mpp);
    };

    // Major-road context behind the route so the shape is recognizable. The
    // status bar + accept sheet are drawn AFTER this, covering any roads that
    // spill above/below the preview viewport (see ui_dashboard draw order).
    static MapScreenData ctx;
    ctx.route = nullptr;
    ctx.routePointCount = 0;
    map_store::renderInto(clat, clon, (float)mpp, cx, cy, 0.0f, ctx);
    for (int i = 0; i < ctx.featureCount; ++i) {
        if (ctx.features[i].cls != MAP_ROAD_MAJOR) continue;
        drawPolyline(ctx.features[i].pts, ctx.features[i].pointCount,
                     {3, 0x00, 0, 0}, fb);
    }

    // Subsample into a fixed buffer so any route length fits.
    static int16_t pts[1024 * 2];
    const int cap = 1024;
    const int stride = (n + cap - 1) / cap;
    int m = 0;
    for (int i = 0; i < n && m < cap; i += stride) {
        double la, lo;
        routes::point(i, la, lo);
        toScreen(la, lo, pts[m * 2], pts[m * 2 + 1]);
        m++;
    }
    // Always include the final point so the end marker lands on the route end.
    {
        double la, lo;
        routes::point(n - 1, la, lo);
        if (m < cap) { toScreen(la, lo, pts[m * 2], pts[m * 2 + 1]); m++; }
    }

    drawPolyline(pts, m, {8, 0x00, 0, 0}, fb);

    // Start (filled dot) and end (ring) markers.
    int16_t sx, sy, ex, ey;
    { double la, lo; routes::point(0, la, lo); toScreen(la, lo, sx, sy); }
    { double la, lo; routes::point(n - 1, la, lo); toScreen(la, lo, ex, ey); }
    epd_fill_circle(sx, sy, 10, 0x00, fb);
    epd_fill_circle(ex, ey, 11, 0xFF, fb);
    for (int r = 7; r <= 11; ++r) epd_draw_circle(ex, ey, r, 0x00, fb);

    // Route-name band across the top (over any road spill), stripped of .gpx.
    epd_fill_rect({0, ui::STATUS_H, W, nameBandH}, 0xFF, fb);
    epd_fill_rect({0, ui::STATUS_H + nameBandH - 2, W, 2}, 0x00, fb);
    const char* rname = routes::activeName();
    if (rname && rname[0]) {
        char nm[40];
        size_t len = strlen(rname);
        const char* dot = strstr(rname, ".gpx");
        if (dot) len = (size_t)(dot - rname);
        if (len > sizeof(nm) - 1) len = sizeof(nm) - 1;
        memcpy(nm, rname, len);
        nm[len] = 0;
        ui::text(&ArialBold_20, W / 2, ui::STATUS_H + 28, nm, fb,
                 EPD_DRAW_ALIGN_CENTER, 0x00);
    }
}
