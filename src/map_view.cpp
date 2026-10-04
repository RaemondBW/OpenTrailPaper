#include "map_view.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>

#include "epdiy.h"
#include "ride_state.h"
#include "routes.h"
#include "map_store.h"
#include "ui_render.h"
#include "vfont_faces.h"

namespace {

// Layout bands (portrait 540x960, design 1f)
constexpr int MAP_TOP = ui::STATUS_H;
constexpr int STRIP_TOP = ui::MAP_STRIP_TOP;   // 3-cell footer below
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
    bool dither;     // draw as a 50% black/white checker so it reads light-grey
                     // even through the fast 1-bit DU refresh
};

Style styleFor(MapFeatureClass cls, float mpp) {
    switch (cls) {
        // Zoomed out (mpp>=16) only arterials survive shedding and there are
        // thousands of them; width<=2 takes the fast Bresenham draw path instead
        // of filled triangles + round caps — a big cut in draw time. Full width
        // up close where a bold arterial reads against the finer roads.
        case MAP_ROAD_MAJOR:     return {mpp >= 16.0f ? 2 : 5, ROAD_INK, 0, 0, false};
        // primary/secondary/tertiary form a width hierarchy (4/3/2) so the road
        // grade reads at a glance. Primary is never shed (thins to 2 at ≥16 like
        // arterial); secondary/tertiary shed at ≥32.
        case MAP_ROAD_PRIMARY:   return {mpp >= 16.0f ? 2 : 4, ROAD_INK, 0, 0, false};
        case MAP_ROAD_SECONDARY: return {3, ROAD_INK, 0, 0, false};
        case MAP_ROAD_TERTIARY:  return {2, ROAD_INK, 0, 0, false};
        case MAP_ROAD_MINOR:     return {2, ROAD_INK, 0, 0, false};
        // Trails: a light-grey dithered thin line (was heavy black dashes).
        case MAP_PATH:           return {1, ROAD_INK, 0, 0, true};
        case MAP_WATER:          return {0, 0, 0, 0, false};   // filled, not stroked
        case MAP_PARK:           return {0, 0, 0, 0, false};   // filled (hatch), not stroked
    }
    return {1, 0x00, 0, 0, false};
}

// Scanline-fill a screen-space polygon with a sparse black dither so it reads as
// a pale grey tint and survives the fast 1-bit DU refresh. Even-odd rule.
// Native-fb-aligned mask (1 byte/fb-byte) of the current frame's dithered dark
// fills (water/parks) — the only regions where DU ghosting settles in, so the
// settle-clean flashes exactly them and nothing else. Rebuilt every map render.
uint8_t* s_ditherMask = nullptr;
size_t s_ditherMaskSize = 0;

void ditherMaskReset() {
    size_t sz = (size_t)(epd_width() / 2) * epd_height();
    if (!s_ditherMask) {
        s_ditherMask = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
        s_ditherMaskSize = s_ditherMask ? sz : 0;
    }
    if (s_ditherMask) memset(s_ditherMask, 0, s_ditherMaskSize);
}

// Mark the native byte holding app pixel (x,y) as dithered fill, only inside the
// map band [MAP_TOP, MAP_BOTTOM) so the status bar/footer never flash.
inline void ditherMaskMark(int x, int y) {
    if (!s_ditherMask || y < MAP_TOP || y >= MAP_BOTTOM) return;
    // INVERTED_PORTRAIT: native_x = app_y, native_y = epd_height()-1 - app_x.
    int nx = y, ny = epd_height() - 1 - x;
    if (nx < 0 || nx >= epd_width() || ny < 0 || ny >= epd_height()) return;
    s_ditherMask[(size_t)ny * (epd_width() / 2) + (nx >> 1)] = 1;
}

// Fill patterns. FILL_DOTS (water) and FILL_HATCH (parks) keep the old bool
// call sites' meaning (false/true).
enum FillPattern : int {
    FILL_DOTS = 0,      // 75% dots — water
    FILL_HATCH = 1,     // 2-px diagonal stripes — parks
    FILL_CHECKER = 2,   // 1-px 50% checker — bike-route band (reads as flat grey,
                        // which is what sets it apart from the textured fills)
};

// FILL_DOTS: dot dither (water). FILL_HATCH: diagonal hatch (parks) — a
// visually distinct texture so green areas don't read the same as water.
// mask=true also records the polygon interior in s_ditherMask (EVERY covered
// pixel, not just the dithered-on ones) for the ghost settle-clean.
void fillDitheredPolygon(const int16_t* pts, int n, uint8_t* fb, int pattern = FILL_DOTS,
                         bool mask = false) {
    if (n < 3) return;
    int minY = 100000, maxY = -100000;
    for (int i = 0; i < n; ++i) {
        int y = pts[i * 2 + 1];
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
    }
    if (minY < 0) minY = 0;
    if (maxY > 959) maxY = 959;
    int xs[64];
    for (int y = minY; y <= maxY; ++y) {
        int cnt = 0;
        for (int i = 0; i < n && cnt < 64; ++i) {
            int j = (i + 1) % n;
            int y0 = pts[i * 2 + 1], y1 = pts[j * 2 + 1];
            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
                int x0 = pts[i * 2], x1 = pts[j * 2];
                xs[cnt++] = x0 + (int)((int64_t)(y - y0) * (x1 - x0) / (y1 - y0));
            }
        }
        for (int a = 0; a < cnt; ++a)
            for (int b = a + 1; b < cnt; ++b)
                if (xs[b] < xs[a]) { int t = xs[a]; xs[a] = xs[b]; xs[b] = t; }
        for (int a = 0; a + 1 < cnt; a += 2) {
            int xL = xs[a] < 0 ? 0 : xs[a];
            int xR = xs[a + 1] > 539 ? 539 : xs[a + 1];
            // Screentones, not flat tone. Both fills are 1-bit patterns, because
            // a true grey value is not displayable on the fast DU refresh — but
            // the PATTERN has to be coarse enough to read as texture. A 1-pixel
            // checkerboard is 50% ink yet resolves as mushy flat grey, which is
            // what parks used to look like: darker than before, but no longer
            // recognisably a screentone the way the water is.
            //
            //   Water — 75% dots (3 of every 2x2). Dense and dark, still clearly
            //           dotted. This is the one that already looked right.
            //   Parks — 50% ink as 2-pixel diagonal stripes on a 4-pixel pitch.
            //           Same darkness as the 1-pixel hatch it replaces, but the
            //           stripes are visible, so parks read as hatched ground and
            //           stay distinct from water at a glance.
            for (int x = xL; x <= xR; ++x) {
                if (mask) ditherMaskMark(x, y);
                bool on = pattern == FILL_HATCH     ? (((x - y) & 3) < 2)
                          : pattern == FILL_CHECKER ? (((x ^ y) & 1) == 0)
                                                    : !((x & 1) && (y & 1));
                // The bike band writes BOTH colours: over a park's hatch or a
                // water fill it then still reads as its own clean grey ribbon
                // instead of melting into the texture beneath.
                if (on) epd_draw_pixel(x, y, 0x00, fb);
                else if (pattern == FILL_CHECKER) epd_draw_pixel(x, y, 0xFF, fb);
            }
        }
    }
}

// Bresenham line that plots only the checkerboard-even pixels, so it reads as a
// light grey on the 1-bit panel and survives the fast DU refresh (unlike a
// solid grey value, which DU snaps to black or white).
void ditherLine(int x0, int y0, int x1, int y1, uint8_t* fb) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if (((x0 ^ y0) & 1) == 0) epd_draw_pixel(x0, y0, 0x00, fb);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Thick segment as a quad (two triangles) plus round caps.
void thickSegment(float x0, float y0, float x1, float y1, int width,
                  uint8_t color, uint8_t* fb) {
    // Fast path for thin strokes — minor roads, paths and rail (width 2) are the
    // vast majority of map features, and one or two Bresenham lines rasterize
    // far cheaper than two filled triangles. This is the bulk of the map draw
    // time on a dense view.
    if (width <= 2) {
        int ix0 = lroundf(x0), iy0 = lroundf(y0);
        int ix1 = lroundf(x1), iy1 = lroundf(y1);
        epd_draw_line(ix0, iy0, ix1, iy1, color, fb);
        if (width == 2) {
            float dx = x1 - x0, dy = y1 - y0;
            float len = sqrtf(dx * dx + dy * dy);
            if (len >= 0.5f) {
                int ox = lroundf(-dy / len), oy = lroundf(dx / len);
                if (ox || oy)
                    epd_draw_line(ix0 + ox, iy0 + oy, ix1 + ox, iy1 + oy, color, fb);
            }
        }
        return;
    }
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
    if (st.dither) {
        ditherLine(lroundf(x0), lroundf(y0), lroundf(x1), lroundf(y1), fb);
        return;
    }
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

// --- cycling layer ------------------------------------------------------------

// Bike-route band: a flat 50% grey ribbon centred under the way, drawn beneath
// all the roads so the road keeps its own weight on top and the band shows as a
// halo either side of it. Grey-on-white is the one road treatment nothing else
// on the map uses (navigation route = solid/dashed black 14 px, water = dots,
// parks = stripes), and the pattern is 1-bit so it survives the DU refresh.
// Width carries the network level. Each segment is a filled quad and each
// joint an octagon; the checker is in absolute screen space so overlaps merge.
void drawBikeBand(const int16_t* pts, int count, int width, uint8_t* fb) {
    const float half = width / 2.0f;
    for (int i = 0; i + 1 < count; ++i) {
        float x0 = pts[i * 2], y0 = pts[i * 2 + 1];
        float x1 = pts[i * 2 + 2], y1 = pts[i * 2 + 3];
        float dx = x1 - x0, dy = y1 - y0;
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.5f) continue;
        float px = -dy / len * half, py = dx / len * half;
        int16_t q[8] = {(int16_t)lroundf(x0 + px), (int16_t)lroundf(y0 + py),
                        (int16_t)lroundf(x1 + px), (int16_t)lroundf(y1 + py),
                        (int16_t)lroundf(x1 - px), (int16_t)lroundf(y1 - py),
                        (int16_t)lroundf(x0 - px), (int16_t)lroundf(y0 - py)};
        fillDitheredPolygon(q, 4, fb, FILL_CHECKER, true);
    }
    // Round the joints so a bend has no notch on its outside. Only up close:
    // zoomed out the band is <= 8 px, a notch is a pixel or two, and the joints
    // were a third of the band's draw time at 16 m/px.
    if (width <= 8) return;
    for (int i = 1; i + 1 < count; ++i) {
        int16_t o[16];
        for (int k = 0; k < 8; ++k) {
            float a = k * (float)M_PI / 4.0f;
            o[k * 2] = (int16_t)lroundf(pts[i * 2] + cosf(a) * half);
            o[k * 2 + 1] = (int16_t)lroundf(pts[i * 2 + 1] + sinf(a) * half);
        }
        fillDitheredPolygon(o, 8, fb, FILL_CHECKER, true);
    }
}

int bikeBandWidth(int level, float mpp) {
    // local / regional / national. Narrower zoomed out, where the roads under
    // it thin to 2 px and a fat band would merge neighbouring streets.
    static const int nearW[4] = {0, 12, 14, 16};
    static const int farW[4] = {0, 6, 8, 10};
    return mpp >= 8.0f ? farW[level & 3] : nearW[level & 3];
}

// Painted lane / track along a road: a dotted line just outside each edge of
// the road, like the lane line on the street. Cheap (two dithered Bresenhams)
// and it never fills between, so the road's own width still reads.
void drawBikeLaneEdges(const int16_t* pts, int count, int roadWidth, uint8_t* fb) {
    const float off = roadWidth / 2.0f + 2.5f;
    for (int i = 0; i + 1 < count; ++i) {
        float x0 = pts[i * 2], y0 = pts[i * 2 + 1];
        float x1 = pts[i * 2 + 2], y1 = pts[i * 2 + 3];
        float dx = x1 - x0, dy = y1 - y0;
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.5f) continue;
        float px = -dy / len * off, py = dx / len * off;
        ditherLine(lroundf(x0 + px), lroundf(y0 + py), lroundf(x1 + px),
                   lroundf(y1 + py), fb);
        ditherLine(lroundf(x0 - px), lroundf(y0 - py), lroundf(x1 - px),
                   lroundf(y1 - py), fb);
    }
}

// POI icons: 24 px. Circles for the comfort stops (water, toilets), squares for
// bike service (repair stand, shop), so the two families read apart before the
// glyph does. Solid black with a white glyph = free to use; a hollow (white,
// black ring) icon = fee / customers only / seasonal. A 2 px white halo keeps
// the icon off whatever road it sits on.
constexpr int POI_R = 12;

// "WC", 15x9, 2-px strokes.
const char* const kWcGlyph[9] = {
    "##....##.######",
    "##....##.######",
    "##....##.##....",
    "##.##.##.##....",
    "##.##.##.##....",
    "##.##.##.##....",
    "########.##....",
    "###..###.######",
    "##....##.######",
};

void drawGlyphRows(const char* const* rows, int nRows, int cx, int cy,
                   uint8_t ink, uint8_t* fb) {
    const int w = (int)strlen(rows[0]);
    const int x0 = cx - w / 2, y0 = cy - nRows / 2;
    for (int r = 0; r < nRows; ++r)
        for (int c = 0; c < w; ++c)
            if (rows[r][c] == '#') epd_draw_pixel(x0 + c, y0 + r, ink, fb);
}

void drawPoiIcon(const MapPoi& p, uint8_t* fb) {
    const int cx = p.x, cy = p.y;
    const bool hollow = (p.flags & MAP_PF_RESTRICTED) != 0;
    const uint8_t bg = hollow ? 0xFF : 0x00, ink = hollow ? 0x00 : 0xFF;
    const bool square = p.type == MAP_POI_REPAIR || p.type == MAP_POI_BIKE_SHOP;
    const int r = POI_R;
    if (square) {
        epd_fill_rect({cx - r - 2, cy - r - 2, 2 * r + 5, 2 * r + 5}, 0xFF, fb);
        epd_fill_rect({cx - r, cy - r, 2 * r + 1, 2 * r + 1}, bg, fb);
        if (hollow) {
            epd_draw_rect({cx - r, cy - r, 2 * r + 1, 2 * r + 1}, 0x00, fb);
            epd_draw_rect({cx - r + 1, cy - r + 1, 2 * r - 1, 2 * r - 1}, 0x00, fb);
        }
    } else {
        epd_fill_circle(cx, cy, r + 2, 0xFF, fb);
        epd_fill_circle(cx, cy, r, bg, fb);
        if (hollow) {
            epd_draw_circle(cx, cy, r, 0x00, fb);
            epd_draw_circle(cx, cy, r - 1, 0x00, fb);
        }
    }
    switch (p.type) {
        case MAP_POI_WATER:   // drop: a round belly under a pointed top
            epd_fill_circle(cx, cy + 3, 5, ink, fb);
            epd_fill_triangle(cx, cy - 8, cx - 5, cy + 2, cx + 5, cy + 2, ink, fb);
            break;
        case MAP_POI_TOILETS:
            drawGlyphRows(kWcGlyph, 9, cx, cy, ink, fb);
            break;
        case MAP_POI_REPAIR: {   // wrench, handle bottom-left to head top-right
            thickSegment(cx - 6, cy + 6, cx + 2, cy - 2, 3, ink, fb);
            epd_fill_circle(cx + 3, cy - 3, 5, ink, fb);
            epd_fill_circle(cx + 6, cy - 6, 3, bg, fb);   // open jaw
            // A pump on the stand: a small plus in the free corner.
            if (p.flags & MAP_PF_PUMP) {
                epd_fill_rect({cx - 8, cy - 7, 7, 2}, ink, fb);
                epd_fill_rect({cx - 5, cy - 10, 2, 7}, ink, fb);
            }
            break;
        }
        case MAP_POI_BIKE_SHOP: {   // bicycle: two wheels and a frame
            epd_draw_circle(cx - 5, cy + 3, 4, ink, fb);
            epd_draw_circle(cx + 5, cy + 3, 4, ink, fb);
            epd_draw_line(cx - 5, cy + 3, cx - 1, cy - 3, ink, fb);
            epd_draw_line(cx - 1, cy - 3, cx + 4, cy - 3, ink, fb);
            epd_draw_line(cx + 4, cy - 3, cx + 5, cy + 3, ink, fb);
            epd_draw_line(cx - 1, cy - 3, cx + 1, cy + 3, ink, fb);
            epd_draw_line(cx - 5, cy + 3, cx + 1, cy + 3, ink, fb);
            epd_draw_line(cx + 4, cy - 3, cx + 3, cy - 6, ink, fb);   // bars
            epd_draw_line(cx - 2, cy - 5, cx + 0, cy - 5, ink, fb);   // saddle
            break;
        }
    }
}

// Icons in priority order with greedy collision culling: a repair stand beats a
// water point beats a toilet beats a shop for the same patch of screen, and an
// icon that would land under the rider marker is skipped (it is drawn over it).
void drawPois(const MapScreenData& map, uint8_t* fb) {
    if (!map.pois || map.poiCount <= 0) return;
    static const uint8_t order[] = {MAP_POI_REPAIR, MAP_POI_WATER,
                                    MAP_POI_TOILETS, MAP_POI_BIKE_SHOP};
    constexpr int kMaxDrawn = 96;
    int16_t drawnX[kMaxDrawn], drawnY[kMaxDrawn];
    int drawn = 0;
    // Zoomed out the icons are thinned harder: at 8 m/px a dense city has an
    // icon every block, and the map under them is the thing being read.
    const int gap = map.metersPerPixel >= 8.0f ? 4 * POI_R : 2 * POI_R + 4;
    // Keep clear of the chrome drawn over the map afterwards — compass, zoom
    // buttons, scale bar, nav banner — which would cut an icon in half.
    const int pad = POI_R + 3;
    const int compY = mapCompassCy(map.navBannerVisible);
    auto underChrome = [&](int x, int y) {
        const int cdx = x - kMapCompass.cx, cdy = y - compY, cr = 28 + pad;
        if (cdx * cdx + cdy * cdy < cr * cr) return true;
        if (x > kMapZoom.zoomX - pad && y > kMapZoom.zoomInY - pad &&
            y < kMapZoom.zoomOutY + kMapZoom.size + pad) return true;
        if (x < 190 + pad && y > MAP_BOTTOM - 48 - pad) return true;   // scale bar
        if (map.navBannerVisible && y < 64 + 138 + pad) return true;
        return false;
    };
    for (uint8_t type : order) {
        for (int i = 0; i < map.poiCount && drawn < kMaxDrawn; ++i) {
            const MapPoi& p = map.pois[i];
            if (p.type != type) continue;
            if (p.y < MAP_TOP + POI_R || p.y > MAP_BOTTOM - POI_R ||
                p.x < POI_R || p.x > 540 - POI_R) continue;
            const int rdx = p.x - map.riderX, rdy = p.y - map.riderY;
            if (rdx * rdx + rdy * rdy < 36 * 36) continue;
            if (underChrome(p.x, p.y)) continue;
            bool hit = false;
            for (int k = 0; k < drawn && !hit; ++k)
                hit = abs(drawnX[k] - p.x) < gap && abs(drawnY[k] - p.y) < gap;
            if (hit) continue;
            drawPoiIcon(p, fb);
            drawnX[drawn] = p.x;
            drawnY[drawn] = p.y;
            drawn++;
        }
    }
}

// "Nearest water" chip: the icon, the distance and an arrow pointing at it in
// screen space. Top-left of the map, under the nav banner / PHONE GPS label
// when those are up.
void drawNearPoiChip(const MapScreenData& map, bool miles, uint8_t* fb) {
    if (!map.nearPoiType) return;
    char buf[16];
    const float d = map.nearPoiM;
    if (miles) {
        if (d < 160.9f) snprintf(buf, sizeof(buf), "%d FT", ((int)(d * 3.281f) + 5) / 10 * 10);
        else snprintf(buf, sizeof(buf), "%.1f MI", d / 1609.34f);
    } else {
        if (d < 1000.0f) snprintf(buf, sizeof(buf), "%d M", ((int)d + 5) / 10 * 10);
        else snprintf(buf, sizeof(buf), "%.1f KM", d / 1000.0f);
    }
    int y = MAP_TOP + 12;
    if (map.navBannerVisible) y = 64 + 138 + 12;
    else if (map.phonePosition) y += 30;
    const int x = 12, h = 40;
    const int tw = ui::textWidth(&Arial_B, buf);
    const int w = 6 + 2 * POI_R + 8 + tw + 10 + 22 + 6;
    epd_fill_rect({x, y, w, h}, 0xFF, fb);
    epd_draw_rect({x, y, w, h}, 0x00, fb);
    epd_draw_rect({x + 1, y + 1, w - 2, h - 2}, 0x00, fb);
    MapPoi icon = {(int16_t)(x + 6 + POI_R), (int16_t)(y + h / 2), map.nearPoiType,
                   map.nearPoiFlags};
    drawPoiIcon(icon, fb);
    ui::text(&Arial_B, x + 6 + 2 * POI_R + 8, y + h / 2 + 9, buf, fb,
             EPD_DRAW_ALIGN_LEFT, 0x00);
    // Arrow: where the water is on THIS screen (track-up rotates it).
    const float a = (map.nearPoiBearingDeg + map.northDeg) * (float)M_PI / 180.0f;
    const float ux = sinf(a), uy = -cosf(a);
    const int ax = x + w - 6 - 11, ay = y + h / 2;
    epd_fill_triangle(ax + lroundf(ux * 11), ay + lroundf(uy * 11),
                      ax + lroundf(-ux * 7 - uy * 7), ay + lroundf(-uy * 7 + ux * 7),
                      ax + lroundf(-ux * 7 + uy * 7), ay + lroundf(-uy * 7 - ux * 7),
                      0x00, fb);
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
    ui::text(&Arial_B, x, y - 10, buf, fb);
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
// Bottom edge of the turn-by-turn banner (ui_dashboard's kNavBanner is
// {0, STATUS_H, 540, 138}); the compass drops below this while navigating.
constexpr int kNavBannerBottom = 64 + 138;

int mapCompassCy(bool navBannerVisible) {
    return navBannerVisible ? kNavBannerBottom + kMapCompass.r + 14
                            : kMapCompass.cy;
}

void ui_map_draw_zoom_button(bool zoomIn, bool pressed, uint8_t* fb) {
    EpdRect r = {kMapZoom.zoomX, zoomIn ? kMapZoom.zoomInY : kMapZoom.zoomOutY,
                 kMapZoom.size, kMapZoom.size};
    const uint8_t face = pressed ? 0x00 : 0xFF;   // pressed = filled ink
    const uint8_t mark = pressed ? 0xFF : 0x00;
    epd_fill_rect(r, face, fb);
    epd_draw_rect(r, 0x00, fb);
    epd_draw_rect({r.x + 1, r.y + 1, r.width - 2, r.height - 2}, 0x00, fb);
    int cx = r.x + r.width / 2, cy = r.y + r.height / 2;
    epd_fill_rect({cx - 14, cy - 2, 28, 5}, mark, fb);
    if (zoomIn) epd_fill_rect({cx - 2, cy - 14, 5, 28}, mark, fb);
}

// Native-fb-aligned mask of the current frame's water/park fills (1 = covered).
// Null until the first map render. Used by the ghost settle-clean.
const uint8_t* ui_map_dither_mask() { return s_ditherMask; }

void ui_render_map_features(const MapScreenData& map, const RideState& s,
                            uint8_t* fb) {
    (void)s;
    // Map features: parks + water under roads, all in grays. Parks (hatch) are
    // the base layer, then water (dots) over them, then trails, then roads in
    // tier order on top. Rail/transit removed by request.
    // mask=true records the dithered fills so the settle-clean flashes exactly
    // those (where DU ghosting settles), not the whole viewport.
    ditherMaskReset();
    for (int i = 0; i < map.parkCount; ++i) {
        fillDitheredPolygon(map.parks[i].pts, map.parks[i].pointCount, fb, FILL_HATCH, true);
    }
    for (int i = 0; i < map.waterCount; ++i) {
        fillDitheredPolygon(map.water[i].pts, map.water[i].pointCount, fb, FILL_DOTS, true);
    }
    // Bike-route bands under every road, lowest network first so a national
    // route's wider band is not covered by a local one's.
    for (int lvl = 1; lvl <= 3; ++lvl) {
        const int bw = bikeBandWidth(lvl, map.metersPerPixel);
        for (int i = 0; i < map.featureCount; ++i) {
            if ((map.features[i].flags & MAP_WF_ROUTE_MASK) != lvl) continue;
            drawBikeBand(map.features[i].pts, map.features[i].pointCount, bw, fb);
        }
    }
    // Back-to-front: higher-grade roads paint on top at intersections.
    const MapFeatureClass order[] = {MAP_PATH, MAP_ROAD_MINOR, MAP_ROAD_TERTIARY,
                                     MAP_ROAD_SECONDARY, MAP_ROAD_PRIMARY,
                                     MAP_ROAD_MAJOR};
    // A dedicated cycleway is a solid-dash black line, a step up from the faint
    // dithered footpath it would otherwise share a class with.
    const Style cyclewayStyle = {2, ROAD_INK, 7, 4, false};
    for (MapFeatureClass cls : order) {
        const Style st = styleFor(cls, map.metersPerPixel);
        for (int i = 0; i < map.featureCount; ++i) {
            const MapPolyline& f = map.features[i];
            if (f.cls != cls) continue;
            const bool cycleway = (f.flags & MAP_WF_CYCLEWAY) != 0 &&
                                  (cls == MAP_PATH || cls == MAP_ROAD_MINOR);
            drawPolyline(f.pts, f.pointCount, cycleway ? cyclewayStyle : st, fb);
            if ((f.flags & MAP_WF_BIKE_LANE) && map.metersPerPixel <= 4.0f)
                drawBikeLaneEdges(f.pts, f.pointCount, st.width, fb);
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

    // POIs over the route: a water stop next to the line being ridden is
    // exactly the one that must not disappear under it.
    drawPois(map, fb);

    drawRider(map.riderX, map.riderY, map.headingDeg, fb);
}

void ui_render_map(const MapScreenData& map, const RideState& s, uint8_t* fb) {
    const int W = epd_rotated_display_width();
    const int H = epd_rotated_display_height();
    char buf[32];

    ui_render_map_features(map, s, fb);
    drawScaleBar(map.metersPerPixel, s.useMiles, fb);
    // The turn banner occupies the top of the map, exactly where the compass
    // lives (compass spans y 78..146; the banner runs to y 202), so the north
    // indicator was drawn underneath it and invisible while navigating. Drop it
    // clear of the banner instead of hiding it.
    drawCompass(kMapCompass.cx, mapCompassCy(map.navBannerVisible),
                map.northDeg, map.trackUp, fb);
    drawNearPoiChip(map, s.useMiles, fb);

    // No map covers this position — tell the rider how to get one instead of
    // showing a blank screen.
    if (!map.hasMap) {
        const int bw = 500, bh = 96;
        const int bx = (W - bw) / 2, by = (H - bh) / 2 - 30;
        epd_fill_rect({bx, by, bw, bh}, 0xFF, fb);
        for (int i = 0; i < 2; ++i)
            epd_draw_rect({bx - i, by - i, bw + 2 * i, bh + 2 * i}, 0x00, fb);
        ui::text(&Arial_B, W / 2, by + 38, "NO MAP HERE", fb,
                 EPD_DRAW_ALIGN_CENTER, 0x00);
        ui::text(&Arial_L, W / 2, by + 70, "Download this area from the app",
                 fb, EPD_DRAW_ALIGN_CENTER, 0x00);
    }

    // Label when the map is centered on the phone's position (device GPS cold).
    // No background box by request — just the text.
    if (map.phonePosition) {
        ui::text(&Arial_L, 21, ui::STATUS_H + 28, "PHONE GPS", fb,
                 EPD_DRAW_ALIGN_LEFT, 0x00);
    }

    // Zoom buttons (design 1f, right edge)
    ui_map_draw_zoom_button(true, false, fb);
    ui_map_draw_zoom_button(false, false, fb);

    // Status bar drawn after the map (epdiy has no clipping)
    epd_fill_rect({0, 0, W, ui::STATUS_H - 3}, 0xFF, fb);
    ui::statusBar(s, fb);

    // Footer: three cells drawn with the SAME primitive as the dashboard, so a
    // rider glancing down sees the same object in both places — caption top-left
    // at the shared size, value+unit centred as a pair.
    epd_fill_rect({0, STRIP_TOP, W, H - STRIP_TOP}, ui::PAPER, fb);
    epd_fill_rect({0, STRIP_TOP, W, ui::RULE_HEAVY}, ui::INK, fb);

    // Dash-page spacing, not flush thirds: the same 24 px screen margin and
    // 12 px gutters the data pages use, so the strip reads as three cells with
    // air rather than one wall-to-wall box.
    const int fy = STRIP_TOP + ui::RULE_HEAVY + ui::GUTTER;
    const int fh = H - fy - ui::GUTTER;
    const int colW = (ui::CONTENT_W - 2 * ui::GUTTER) / 3;

    // Fields come from the rider's config (`map` line in dashboard.cfg).
    // One override stays: while navigating, if no cell already shows ROUTE
    // LEFT, it replaces the third — the strip's oldest behaviour, and the one
    // number a rider on a route actually wants.
    uint8_t f3[3] = {map.stripFields[0], map.stripFields[1], map.stripFields[2]};
    if (map.showRemaining) {
        bool has = f3[0] == DF_ROUTE_LEFT || f3[1] == DF_ROUTE_LEFT ||
                   f3[2] == DF_ROUTE_LEFT;
        if (!has) f3[2] = DF_ROUTE_LEFT;
    }

    char vals[3][24];
    const char* units3[3];
    const char* labels[3];
    for (int c = 0; c < 3; ++c) {
        if (f3[c] >= DF_COUNT) f3[c] = DF_SPEED;
        units3[c] = "";
        dashFieldValue(f3[c], s, vals[c], sizeof(vals[c]), &units3[c]);
        // Time fields render compact H:MM here, as the strip always did before
        // it was configurable. The full H:MM:SS is a dash-cell luxury: in a
        // 156 px strip cell its worst case ("88:88:88") drags the WHOLE row's
        // shared face down to the smallest sizes — seconds nobody reads at a
        // glance costing half the type size of all three cells.
        if (f3[c] == DF_RIDE_TIME || f3[c] == DF_MOVING_TIME) {
            uint32_t sec = f3[c] == DF_RIDE_TIME ? s.elapsedS : s.movingS;
            snprintf(vals[c], sizeof(vals[c]), "%lu:%02lu",
                     (unsigned long)(sec / 3600), (unsigned long)((sec / 60) % 60));
        }
        labels[c] = dashFieldLabel(f3[c]);
    }


    // The strip's own worst-case sizing strings: dashSizingHint's, except the
    // compact time format above.
    auto stripHint = [](uint8_t f) {
        return (f == DF_RIDE_TIME || f == DF_MOVING_TIME) ? "88:88"
                                                          : dashSizingHint(f);
    };

    // One caption size across the strip, as on the data page.
    const EpdFont* lf = ui::kLabelLadder[ui::LABEL_LADDER_N - 1];
    for (int k = 0; k < ui::LABEL_LADDER_N; ++k) {
        bool fits = true;
        for (int c = 0; c < 3; ++c)
            if (ui::labelWidth(ui::kLabelLadder[k], labels[c]) >
                colW - 2 * ui::CELL_PAD) fits = false;
        if (fits) { lf = ui::kLabelLadder[k]; break; }
    }

    // Common value face across the three, so the strip reads as one row —
    // sized against each field's WORST-CASE string, not the live value, so a
    // number crossing a digit boundary can never resize the strip mid-ride.
    // The unit no longer sits beside the value (in a 124 px inner cell it
    // cost two ladder steps of type size); the cell is a three-band column:
    // caption high, big bare number, small unit on the bottom edge.
    const int capBand = 34;    // caption band, from the cell top
    const int unitBand = 26;   // unit band, from the cell bottom
    // The value's side padding is 6, not CELL_PAD's 16: nothing sits beside
    // the number, and those 20 px are exactly one ladder step of type size.
    const int valuePad = 6;
    int vi = 0;
    for (int c = 0; c < 3; ++c) {
        const int idx = ui::valueFontIndex(ui::kValueLadder,
                                           stripHint(f3[c]),
                                           colW - 2 * valuePad,
                                           fh - capBand - unitBand, 0);
        if (idx > vi) vi = idx;
    }
    const EpdFont* vf = ui::kValueLadder[vi];
    for (int c = 0; c < 3; ++c) {
        EpdRect r = {ui::CONTENT_X + c * (colW + ui::GUTTER), fy, colW, fh};
        epd_draw_rect(r, ui::INK, fb);
        epd_draw_rect({r.x + 1, r.y + 1, r.width - 2, r.height - 2}, ui::INK,
                      fb);
        const int cx = r.x + r.width / 2;
        // Left-aligned caption, the dash cells' own idiom: label() centres on
        // a point, so hand it the tracked width's midpoint.
        const int lw = ui::labelWidth(lf, labels[c]);
        ui::label(r.x + ui::CELL_PAD + lw / 2, r.y + 24, labels[c], fb,
                  ui::INK, lf);
        // Value centred in the band between caption and unit. Impact digit
        // faces carry cap height in ascender, so baseline = centre + asc/2
        // is optical centre.
        const int bandTop = r.y + capBand;
        const int bandBot = r.y + fh - unitBand;
        const int base = (bandTop + bandBot) / 2 + vf->ascender / 2;
        ui::text(vf, cx, base, vals[c], fb, EPD_DRAW_ALIGN_CENTER);
        if (units3[c][0])
            ui::text(&Arial_L, cx, r.y + fh - 10, units3[c], fb,
                     EPD_DRAW_ALIGN_CENTER, ui::DARK);
        // Greyed like a dash cell when the source is gone — a strip cell must
        // never show a live-looking number from a dead sensor either.
        if (!dashFieldAvailable(f3[c], s) && !s.showOffline)
            ui::fillTone({r.x + 3, bandTop, r.width - 6, bandBot - bandTop},
                         ui::TONE_33, fb);
    }
}

// Whole-route preview for the "Start navigation?" accept page: fits the entire
// route into the area above the prompt sheet so the rider can recognize it
// before accepting. Independent of the live map zoom/center.
void ui_render_route_preview(uint8_t* fb) {
    const int n = routes::pointCount();
    if (n < 2) return;
    const int W = epd_rotated_display_width();


    // Viewport: below the status bar, above the accept sheet. The sheet is 520 px
    // tall now, so fitting to 600 put the lower third of the route behind it —
    // the rider was asked to accept a route they could only half see.
    const int H = epd_rotated_display_height();
    const int top = ui::STATUS_H + 8, bot = (H - 520) - 16;
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
    // Parks (hatch) + water (dots) under the roads, so the coast/bay + green
    // areas read — matches the map screen. Spill above/below the viewport is
    // masked by the status bar + accept sheet drawn afterwards.
    for (int i = 0; i < ctx.parkCount; ++i) {
        fillDitheredPolygon(ctx.parks[i].pts, ctx.parks[i].pointCount, fb, FILL_HATCH);
    }
    for (int i = 0; i < ctx.waterCount; ++i) {
        fillDitheredPolygon(ctx.water[i].pts, ctx.water[i].pointCount, fb);
    }
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
}
