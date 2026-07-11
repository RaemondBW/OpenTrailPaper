#include "map_view.h"

#include <cmath>
#include <cstdio>

#include "epdiy.h"
#include "ride_state.h"
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

void drawScaleBar(float metersPerPixel, uint8_t* fb) {
    const int targets[] = {50, 100, 200, 500, 1000, 2000};
    int meters = targets[0];
    for (int t : targets) {
        if (t / metersPerPixel <= 160) meters = t;
    }
    int px = (int)(meters / metersPerPixel);
    int x = 20, y = MAP_BOTTOM - 26;
    epd_fill_rect({x, y, px, 4}, 0x00, fb);
    char buf[16];
    if (meters >= 1000) snprintf(buf, sizeof(buf), "%d KM", meters / 1000);
    else snprintf(buf, sizeof(buf), "%d M", meters);
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

void ui_render_map(const MapScreenData& map, const RideState& s, uint8_t* fb) {
    const int W = epd_rotated_display_width();
    const int H = epd_rotated_display_height();
    char buf[32];

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
    drawScaleBar(map.metersPerPixel, fb);
    drawCompass(kMapCompass.cx, kMapCompass.cy, map.northDeg, map.trackUp, fb);

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

    ui::label(colW / 2, STRIP_TOP + 44, "SPEED", fb);
    snprintf(buf, sizeof(buf), "%.1f", s.speedKmh);
    ui::valueWithUnit(&Impact_40, 8, colW - 8, H - 40, buf, "", fb);

    ui::label(colW + colW / 2, STRIP_TOP + 44, "DISTANCE", fb);
    snprintf(buf, sizeof(buf), "%.1f", s.distanceM / 1000.0);
    ui::valueWithUnit(&Impact_40, colW + 8, 2 * colW - 8, H - 40, buf, "", fb);

    if (map.showRemaining) {
        ui::label(2 * colW + colW / 2, STRIP_TOP + 44, "LEFT KM", fb);
        snprintf(buf, sizeof(buf), "%.1f", map.remainingKm);
    } else {
        ui::label(2 * colW + colW / 2, STRIP_TOP + 44, "TIME", fb);
        snprintf(buf, sizeof(buf), "%lu:%02lu", (unsigned long)(s.elapsedS / 3600),
                 (unsigned long)((s.elapsedS / 60) % 60));
    }
    ui::valueWithUnit(&Impact_40, 2 * colW + 8, W - 8, H - 40, buf, "", fb);
}
