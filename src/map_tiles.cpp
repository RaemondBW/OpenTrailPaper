#include "map_tiles.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

namespace {

// Scratch capacity. Sized against MEASURED peaks, not guesses: 5,070 frames
// over a real res-6 tile set, at every zoom on the ladder and five headings
// (tools/map_test/run_tilescene.sh with TILESCENE_SWEEP=1) reported
//
//   road points  40,398     water points  2,101
//   road polys   18,216     water polys       91
//
// Two things that reads off, both of which set the sizes below.
//
// POINTS were 4x oversized. After the ~2 px screen-space decimation the kept
// points are bounded by the SCREEN, not by how many tiles are in view — a wider
// view pulls more tiles but each contributes proportionally fewer points — so
// this does not grow with coverage and 2x the measured peak is real headroom.
//
// POLYS are the binding budget — 91% full at the old 20,000, so they go UP
// rather than down. The scratch is dominated by POLYLINE RECORDS, not by
// geometry: 18,216 records of 12 bytes hold 40,398 points of 4, because a road
// split at sub-tile boundaries and then decimated averages 2.2 points. Overflow
// abandons the rest of a tile, and since tiles are projected nearest-first that
// shows up as the far edges of the map thinning out.
constexpr int MAX_POINTS = 80000;
constexpr int MAX_POLYS = 24000;
// Water (WTR2) and park (PRK2) polygons get their own smaller scratch. Water is
// trimmed to 12x its measured peak; parks keep the old size because the tile set
// measured predates the parks layer and has none to measure.
constexpr int MAX_WATER_POINTS = 12000;
constexpr int MAX_WATER_POLYS = 512;
constexpr int MAX_PARK_POINTS = 24000;
constexpr int MAX_PARK_POLYS = 512;
// Cycling POIs (from each tile's .poi file). Dense downtown SF holds ~800 in
// 7 tiles; the widest zoom that shows any (8 m/px) sees a ~4x8 km window.
constexpr int MAX_POIS = 256;

// The single "primary" blob used by the embedded map + route overlay path.
const uint8_t* blob = nullptr;
size_t blobLen = 0;

int16_t* pts = nullptr;
MapPolyline* polys = nullptr;
int16_t* waterPts = nullptr;
MapPolyline* waterPolys = nullptr;
int16_t* parkPts = nullptr;
MapPolyline* parkPolys = nullptr;
MapPoi* poiBuf = nullptr;

// Shared append cursors across a multi-tile frame (map_store drives these
// via beginProject / projectBlobInto / endProject).
int g_usedPts = 0, g_usedPolys = 0;
int g_clsKept[7] = {0, 0, 0, 0, 0, 0, 0};   // diag: polys kept per class this frame
int g_usedWaterPts = 0, g_usedWaterPolys = 0;
int g_usedParkPts = 0, g_usedParkPolys = 0;
int g_usedPois = 0;
map_tiles::MapProjectStats g_stats = {};

// The screen rectangle a projected feature has to reach to be worth keeping,
// with a margin so a thick stroke centred just off-panel still shows its edge.
constexpr int VP_X0 = -50, VP_X1 = 590, VP_Y0 = -50, VP_Y1 = 1010;

// Could the segment a->b put ink on the panel? A bounding-box test: exact for
// axis-aligned work, and for a diagonal that misses it only costs one clipped
// draw. What matters is that it never says no to a segment that crosses.
inline bool segmentOnScreen(int ax, int ay, int bx, int by) {
    int lo = ax < bx ? ax : bx, hi = ax < bx ? bx : ax;
    if (hi <= VP_X0 || lo >= VP_X1) return false;
    lo = ay < by ? ay : by; hi = ay < by ? by : ay;
    return hi > VP_Y0 && lo < VP_Y1;
}

void* bigAlloc(size_t n) {
#ifdef ARDUINO
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
#else
    return malloc(n);
#endif
}

template <typename T>
T rd(const uint8_t* p) {
    T v;
    memcpy(&v, p, sizeof(T));
    return v;
}

// Zoomed out: shed detail to keep the feature count + draw time bounded and the
// overview legible. Paths go first (>=4 m/px), then minor/residential (>=16),
// then secondary/tertiary (>=32). Primary and arterial never shed — they carry
// the overview at the widest zooms.
//
// Bike infrastructure outlives its road class: a signed bike route is the
// thing a rider zooms out to FOLLOW, so regional/national routes never shed and
// local ones last to 16 m/px; a dedicated cycleway lasts to 8 m/px. Only ever
// keeps MORE than the plain class rule — a flag can never shed a primary road.
inline bool shedAtZoom(uint8_t cls, uint8_t flags, float mpp) {
    bool shed = (mpp >= 4.0f && cls == MAP_PATH) ||
                (mpp >= 16.0f && cls == MAP_ROAD_MINOR) ||
                (mpp >= 32.0f &&
                 (cls == MAP_ROAD_SECONDARY || cls == MAP_ROAD_TERTIARY));
    if (!shed || !flags) return shed;
    const int lvl = flags & MAP_WF_ROUTE_MASK;
    if (lvl >= 2) return false;
    if (lvl == 1) return mpp >= 32.0f;
    if (flags & MAP_WF_CYCLEWAY) return mpp >= 16.0f;
    return true;
}

// Which POI types a zoom shows. Icons are ~24 px; past 8 m/px they would carpet
// the screen. At 8 m/px only water and repair stands — the two a rider plans a
// detour for; toilets and shops come back at 4 m/px.
inline bool poiAtZoom(uint8_t type, float mpp) {
    if (mpp <= 4.0f) return type >= MAP_POI_WATER && type <= MAP_POI_BIKE_SHOP;
    if (mpp <= 8.0f) return type == MAP_POI_WATER || type == MAP_POI_REPAIR;
    return false;
}

}  // namespace

namespace map_tiles {

static bool ensureScratch() {
    if (!pts) {
        pts = (int16_t*)bigAlloc(MAX_POINTS * 2 * sizeof(int16_t));
        polys = (MapPolyline*)bigAlloc(MAX_POLYS * sizeof(MapPolyline));
        waterPts = (int16_t*)bigAlloc(MAX_WATER_POINTS * 2 * sizeof(int16_t));
        waterPolys = (MapPolyline*)bigAlloc(MAX_WATER_POLYS * sizeof(MapPolyline));
        parkPts = (int16_t*)bigAlloc(MAX_PARK_POINTS * 2 * sizeof(int16_t));
        parkPolys = (MapPolyline*)bigAlloc(MAX_PARK_POLYS * sizeof(MapPolyline));
        poiBuf = (MapPoi*)bigAlloc(MAX_POIS * sizeof(MapPoi));
    }
    return pts && polys && waterPts && waterPolys && parkPts && parkPolys &&
           poiBuf;
}

bool load(const uint8_t* data, size_t len) {
    if (len < 36 || memcmp(data, "EBM2", 4) != 0) return false;
    blob = data;
    blobLen = len;
    return ensureScratch();
}

bool loaded() { return blob != nullptr && pts != nullptr; }

void beginProject(MapScreenData& out) {
    ensureScratch();
    out.features = polys;
    out.featureCount = 0;
    out.water = waterPolys;
    out.waterCount = 0;
    out.parks = parkPolys;
    out.parkCount = 0;
    out.pois = poiBuf;
    out.poiCount = 0;
    g_usedPois = 0;
    g_usedPts = 0;
    g_usedPolys = 0;
    g_usedWaterPts = 0;
    g_usedWaterPolys = 0;
    g_usedParkPts = 0;
    g_usedParkPolys = 0;
    g_stats = {};
    for (int i = 0; i < 7; ++i) g_clsKept[i] = 0;
}

int projectedPolyCount() { return g_usedPolys; }
void projectedClassCounts(int out[7]) { for (int i = 0; i < 7; ++i) out[i] = g_clsKept[i]; }

MapProjectStats projectStats() {
    MapProjectStats s = g_stats;
    s.usedPoints = g_usedPts;
    s.usedPolys = g_usedPolys;
    s.usedWaterPoints = g_usedWaterPts;
    s.usedParkPoints = g_usedParkPts;
    s.capPoints = MAX_POINTS;
    s.capPolys = MAX_POLYS;
    s.capWaterPoints = MAX_WATER_POINTS;
    s.capWaterPolys = MAX_WATER_POLYS;
    s.capParkPoints = MAX_PARK_POINTS;
    s.capParkPolys = MAX_PARK_POLYS;
    s.usedPois = g_usedPois;
    s.capPois = MAX_POIS;
    return s;
}

void endProject(MapScreenData& out) {
    out.featureCount = g_usedPolys;
    out.waterCount = g_usedWaterPolys;
    out.parkCount = g_usedParkPolys;
    out.poiCount = g_usedPois;
}

// Project a single EBM1 blob (with its own grid header) into the shared
// scratch buffers, appending at the current cursors. Safe to call for
// several tile blobs in one frame between beginProject / endProject.
void projectBlobInto(const uint8_t* b, size_t bLen, double lat, double lon,
                     float metersPerPixel, int centerX, int centerY,
                     float rotateDeg) {
    if (!pts || bLen < 36 || memcmp(b, "EBM2", 4) != 0) return;

    double gridLat0 = rd<double>(b + 4);
    double gridLon0 = rd<double>(b + 12);
    double tileDeg = rd<double>(b + 20);
    int32_t gridNx = rd<int32_t>(b + 28);
    int32_t gridNy = rd<int32_t>(b + 32);
    const uint8_t* indexBase = b + 36;

    double midLat = gridLat0 + tileDeg * gridNy / 2.0;
    double kx = 111320.0 * cos(midLat * M_PI / 180.0);
    double ky = 110540.0;

    float rc = 1, rs = 0;
    if (rotateDeg != 0) {
        rc = cosf(rotateDeg * (float)M_PI / 180.0f);
        rs = sinf(rotateDeg * (float)M_PI / 180.0f);
    }

    // Viewport extent in meters around the center (portrait 540x960 max)
    const float halfWm = 300 * metersPerPixel;
    const float halfHm = 520 * metersPerPixel;
    // Reciprocal so the hot per-point projection uses a multiply, not an FPU
    // divide (thousands of points per frame).
    const float invMpp = 1.0f / metersPerPixel;

    // Position in grid meters
    double px = (lon - gridLon0) * kx;
    double py = (lat - gridLat0) * ky;

    double tileWm = tileDeg * kx, tileHm = tileDeg * ky;
    int tx0 = (int)floor((px - halfWm) / tileWm);
    int tx1 = (int)floor((px + halfWm) / tileWm);
    int ty0 = (int)floor((py - halfHm) / tileHm);
    int ty1 = (int)floor((py + halfHm) / tileHm);

    int usedPts = g_usedPts, usedPolys = g_usedPolys;

    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            if (tx < 0 || tx >= gridNx || ty < 0 || ty >= gridNy) continue;
            uint32_t off = rd<uint32_t>(indexBase + (ty * gridNx + tx) * 8);
            uint32_t len = rd<uint32_t>(indexBase + (ty * gridNx + tx) * 8 + 4);
            if (!off || off + len > bLen) continue;

            // Tile origin relative to position, in screen px
            float originX = centerX + (float)((tx * tileWm - px) / metersPerPixel);
            float originY = centerY - (float)((ty * tileHm - py) / metersPerPixel);

            const uint8_t* p = b + off;
            const uint8_t* end = p + len;
            uint16_t count = rd<uint16_t>(p);
            p += 2;

            // Way-flag trailer: exactly `count` bytes after the last polyline
            // (docs/mapgen.js). Found by hopping the record headers — no point
            // is read — and recognised by its exact length, which a tile
            // without one (every tile built before it) can never have.
            const uint8_t* wflags = nullptr;
            {
                const uint8_t* h = p;
                uint16_t k = 0;
                while (k < count && h + 3 <= end) {
                    h += 3 + (size_t)rd<uint16_t>(h + 1) * 4;
                    ++k;
                }
                if (k == count && h <= end && (size_t)(end - h) == count)
                    wflags = h;
            }

            for (uint16_t i = 0; i < count && p + 3 <= end; ++i) {
                uint8_t cls = *p;
                uint16_t n = rd<uint16_t>(p + 1);
                p += 3;
                if (p + n * 4 > end) goto done;
                const uint8_t wf = wflags ? wflags[i] : 0;

                // Dropping what this zoom sheds keeps the whole city inside
                // the scratch buffers so the north isn't truncated.
                if (shedAtZoom(cls, wf, metersPerPixel)) {
                    p += n * 4;
                    continue;
                }

                if (usedPolys >= MAX_POLYS || usedPts + n > MAX_POINTS) {
                    // The scratch is full, so the rest of this sub-tile — and
                    // every sub-tile after it — is lost. Which geometry that is
                    // depends on iteration order, not on what matters.
                    g_stats.roadsDropped += count - i;
                    g_stats.blobsTruncated++;
                    goto done;
                }

                // Project + viewport reject + screen-space decimation in one
                // pass. The tile keeps 3 m detail; zoomed out that is far below a
                // pixel, so most points land on top of each other. Dropping any
                // point within ~2 px of the last kept one collapses the geometry
                // to what the current zoom can show — a big cut in segments (and
                // draw time), invisible on glass. First/last points always kept
                // so roads still connect.
                int16_t* dst = pts + usedPts * 2;
                int kept = 0;
                int lastKx = -30000, lastKy = -30000;
                bool touchesViewport = false;
                for (uint16_t j = 0; j < n; ++j) {
                    int16_t mx = rd<int16_t>(p + j * 4);
                    int16_t my = rd<int16_t>(p + j * 4 + 2);
                    float sx = originX + mx * invMpp;
                    float sy = originY - my * invMpp;
                    if (rotateDeg != 0) {
                        float dx = sx - centerX, dy = sy - centerY;
                        sx = centerX + dx * rc - dy * rs;
                        sy = centerY + dx * rs + dy * rc;
                    }
                    // clamp to int16 to be safe at deep zoom-out
                    if (sx < -20000) sx = -20000;
                    if (sx > 20000) sx = 20000;
                    if (sy < -20000) sy = -20000;
                    if (sy > 20000) sy = 20000;
                    int ix = (int)lroundf(sx), iy = (int)lroundf(sy);
                    bool isLast = (j == n - 1);
                    if (kept > 0 && !isLast) {
                        int adx = ix - lastKx, ady = iy - lastKy;
                        if (adx * adx + ady * ady < 4) continue;   // within ~2 px
                    }
                    // Keep the feature if any SEGMENT of it reaches the screen,
                    // not if any VERTEX lands on it. A vertex test throws away
                    // every line that crosses the viewport between two distant
                    // points — at 2 m/px one stored tile is 2800 px wide, so a
                    // road (or a tile-sized water ring) routinely spans the whole
                    // screen with both ends off it. That is what left hard
                    // straight edges where a bay simply stopped being drawn.
                    if (kept > 0 && segmentOnScreen(lastKx, lastKy, ix, iy))
                        touchesViewport = true;
                    dst[kept * 2] = (int16_t)ix;
                    dst[kept * 2 + 1] = (int16_t)iy;
                    lastKx = ix; lastKy = iy;
                    kept++;
                }
                p += n * 4;

                if (kept >= 2 && !touchesViewport) g_stats.roadsOffscreen++;
                if (kept < 2 || !touchesViewport) continue;
                polys[usedPolys].cls = (MapFeatureClass)cls;
                polys[usedPolys].flags = wf;
                polys[usedPolys].pts = dst;
                polys[usedPolys].pointCount = kept;
                usedPolys++;
                usedPts += kept;
                if (cls < 7) g_clsKept[cls]++;
            }
        }
    }
done:
    g_usedPts = usedPts;
    g_usedPolys = usedPolys;

    // --- water polygons (WTR2), stored after the road data + optional ELV1.
    // Points are metres E/N of the tile origin (gridLat0/gridLon0), like roads
    // but relative to the tile corner instead of a sub-tile. Projected here and
    // filled (dithered) by the renderer under the roads.
    size_t maxEnd = 36 + (size_t)gridNx * gridNy * 8;
    for (int k = 0; k < gridNx * gridNy; ++k) {
        uint32_t off = rd<uint32_t>(indexBase + (size_t)k * 8);
        uint32_t l = rd<uint32_t>(indexBase + (size_t)k * 8 + 4);
        if (off && (size_t)off + l > maxEnd) maxEnd = (size_t)off + l;
    }
    size_t wp = maxEnd;
    if (wp + 44 <= bLen && memcmp(b + wp, "ELV1", 4) == 0) {
        int32_t gw = rd<int32_t>(b + wp + 4), gh = rd<int32_t>(b + wp + 8);
        wp += 44 + (size_t)gw * gh * 2;   // skip the elevation block
    }
    if (wp + 6 > bLen || memcmp(b + wp, "WTR2", 4) != 0) return;
    const uint8_t* q = b + wp;
    const uint8_t* wend = b + bLen;

    // Parse a filled-polygon section: <4-byte magic><u16 count>, then each
    // polygon as <u16 pointCount><i16 x,y ...> in metres E/N of the grid origin.
    // Points project exactly like the roads. q is always advanced past every
    // polygon (even when the scratch is full) so the next section stays aligned.
    // Assumes the magic at q was already matched by the caller.
    auto parseFill = [&](int16_t* dstPts, MapPolyline* dstPolys,
                         int& usedPts, int& usedPolys, int maxPts, int maxPolys,
                         MapFeatureClass cls, int& dropped, int& offscreen) {
        uint16_t polyCount = rd<uint16_t>(q + 4);
        q += 6;
        for (int pi = 0; pi < polyCount && q + 2 <= wend; ++pi) {
            uint16_t wn = rd<uint16_t>(q);
            q += 2;
            if (q + (size_t)wn * 4 > wend) break;
            const uint8_t* poly = q;
            q += (size_t)wn * 4;                 // always advance to next poly
            // Room is checked against what the ring COSTS AFTER decimation, not
            // against its stored point count. A coastline stores thousands of
            // 3 m points and keeps a few dozen of them at a wide zoom; reserving
            // the stored count filled the budget with rings that were never
            // going to use it, and the water past that point simply vanished.
            const int room = maxPts - usedPts;
            if (usedPolys >= maxPolys || room < 3) { dropped++; continue; }
            int16_t* dst = dstPts + usedPts * 2;
            int kept = 0;
            bool overflow = false;
            int lastKx = -30000, lastKy = -30000;
            int minX = 30000, maxX = -30000, minY = 30000, maxY = -30000;
            for (uint16_t j = 0; j < wn; ++j) {
                float sx = centerX + ((float)rd<int16_t>(poly + j * 4) - (float)px) * invMpp;
                float sy = centerY - ((float)rd<int16_t>(poly + j * 4 + 2) - (float)py) * invMpp;
                if (rotateDeg != 0) {
                    float dx = sx - centerX, dy = sy - centerY;
                    sx = centerX + dx * rc - dy * rs;
                    sy = centerY + dx * rs + dy * rc;
                }
                if (sx < -20000) sx = -20000;
                if (sx > 20000) sx = 20000;
                if (sy < -20000) sy = -20000;
                if (sy > 20000) sy = 20000;
                int ix = (int)lroundf(sx), iy = (int)lroundf(sy);
                // Same ~2 px screen-space decimation the roads get. A ring is
                // stored at 3 m detail; at 32 m/px a whole coastline collapses
                // to a few dozen points, which is what makes it affordable to
                // keep every tile's water at the widest zoom.
                if (kept > 0) {
                    int adx = ix - lastKx, ady = iy - lastKy;
                    if (adx * adx + ady * ady < 4) continue;
                }
                if (kept == room) { overflow = true; break; }   // won't fit whole
                if (ix < minX) minX = ix;
                if (ix > maxX) maxX = ix;
                if (iy < minY) minY = iy;
                if (iy > maxY) maxY = iy;
                dst[kept * 2] = (int16_t)ix;
                dst[kept * 2 + 1] = (int16_t)iy;
                lastKx = ix; lastKy = iy;
                kept++;
            }
            // A ring drawn half-finished is a shape that was never there, so a
            // ring that does not fit is dropped whole.
            if (overflow) { dropped++; continue; }
            // A FILL is kept when its bounding box overlaps the panel, not when
            // one of its vertices lands on it. The rings here are region-scale —
            // a bay clipped to its tile — so zoomed in, the ring that should
            // flood the whole screen has every vertex kilometres off it. The
            // vertex test dropped exactly those, which is why the water ended in
            // hard straight lines at tile edges and vanished entirely once the
            // rider was inside a big one.
            const bool overlaps = maxX > VP_X0 && minX < VP_X1 &&
                                  maxY > VP_Y0 && minY < VP_Y1;
            if (kept >= 3 && !overlaps) offscreen++;
            if (kept >= 3 && overlaps) {
                dstPolys[usedPolys].cls = cls;
                dstPolys[usedPolys].flags = 0;
                dstPolys[usedPolys].pts = dst;
                dstPolys[usedPolys].pointCount = kept;
                usedPolys++;
                usedPts += kept;
            }
        }
    };

    parseFill(waterPts, waterPolys, g_usedWaterPts, g_usedWaterPolys,
              MAX_WATER_POINTS, MAX_WATER_POLYS, MAP_WATER,
              g_stats.waterDropped, g_stats.waterOffscreen);

    // Parks (PRK2) follow the water section. Older tiles omit it — skip cleanly.
    if (q + 6 <= wend && memcmp(q, "PRK2", 4) == 0) {
        parseFill(parkPts, parkPolys, g_usedParkPts, g_usedParkPolys,
                  MAX_PARK_POINTS, MAX_PARK_POLYS, MAP_PARK,
                  g_stats.parksDropped, g_stats.parksOffscreen);
    }

    // Extension sections: <magic><u32 length><payload>, skipped by length when
    // unknown so later additions never need another firmware to stay readable.
    while (q + 8 <= wend) {
        const uint32_t secLen = rd<uint32_t>(q + 4);
        const uint8_t* body = q + 8;
        if (secLen > (size_t)(wend - body)) break;   // truncated / not a section
        // No section is defined yet: cycling POIs live in a separate .poi
        // file per tile (projectPoisInto), not in the map. This loop only
        // keeps future sections skippable.
        q = body + secLen;
    }
}

void project(double lat, double lon, float metersPerPixel, int centerX,
             int centerY, float rotateDeg, MapScreenData& out) {
    beginProject(out);
    if (blob) {
        projectBlobInto(blob, blobLen, lat, lon, metersPerPixel, centerX,
                        centerY, rotateDeg);
    }
    endProject(out);
}

// .poi file (docs/mapgen.js buildPoi), little-endian:
//    0  'EPOI'
//    4  u8  version (1)
//    5  u8  recordSize (6; a later version may append fields to a record)
//    6  u16 count
//    8  u64 H3 cell the file belongs to (0 = not an H3 tile)
//   16  f64 lat0, f64 lon0   origin of x/y
//   32  f32 kx, f32 ky       metres per degree of lon / lat used for x/y
//   40  count x { u8 type, u8 flags, i16 x, i16 y }   x/y metres E/N of origin
// Self-describing: nothing about the map tile is needed to place a POI.
bool poiFileValid(const uint8_t* b, size_t len) {
    if (len < POI_HEADER_LEN || memcmp(b, "EPOI", 4) != 0 || b[4] != 1) return false;
    const uint8_t recSize = b[5];
    const uint16_t count = rd<uint16_t>(b + 6);
    return recSize >= 6 && POI_HEADER_LEN + (size_t)count * recSize <= len &&
           rd<float>(b + 32) > 0 && rd<float>(b + 36) > 0;
}

void projectPoisInto(const uint8_t* b, size_t len, double lat, double lon,
                     float metersPerPixel, int centerX, int centerY,
                     float rotateDeg) {
    if (!poiBuf || !poiFileValid(b, len)) return;
    if (!poiAtZoom(MAP_POI_WATER, metersPerPixel)) return;   // none at this zoom
    const uint8_t recSize = b[5];
    const uint16_t count = rd<uint16_t>(b + 6);
    const double lat0 = rd<double>(b + 16), lon0 = rd<double>(b + 24);
    const double kx = rd<float>(b + 32), ky = rd<float>(b + 36);
    // Rider position in the file's own metre frame.
    const double px = (lon - lon0) * kx, py = (lat - lat0) * ky;
    const float invMpp = 1.0f / metersPerPixel;
    float rc = 1, rs = 0;
    if (rotateDeg != 0) {
        rc = cosf(rotateDeg * (float)M_PI / 180.0f);
        rs = sinf(rotateDeg * (float)M_PI / 180.0f);
    }
    const uint8_t* r = b + POI_HEADER_LEN;
    for (uint16_t i = 0; i < count; ++i, r += recSize) {
        const uint8_t type = r[0];
        if (!poiAtZoom(type, metersPerPixel)) continue;
        float sx = centerX + (float)(rd<int16_t>(r + 2) - px) * invMpp;
        float sy = centerY - (float)(rd<int16_t>(r + 4) - py) * invMpp;
        if (rotateDeg != 0) {
            float dx = sx - centerX, dy = sy - centerY;
            sx = centerX + dx * rc - dy * rs;
            sy = centerY + dx * rs + dy * rc;
        }
        if (sx < -16 || sx > 556 || sy < -16 || sy > 976) continue;
        const int ix = (int)lroundf(sx), iy = (int)lroundf(sy);
        // Builders store each POI in the one H3 cell containing it, so a
        // duplicate should not happen — but a builder that keeps the tile's
        // whole bounding box (overlapping its neighbours') is still drawn once.
        bool dup = false;
        for (int k = 0; k < g_usedPois && !dup; ++k)
            dup = poiBuf[k].type == type && abs(poiBuf[k].x - ix) <= 1 &&
                  abs(poiBuf[k].y - iy) <= 1;
        if (dup) continue;
        if (g_usedPois >= MAX_POIS) { g_stats.poisDropped++; continue; }
        poiBuf[g_usedPois++] = {(int16_t)ix, (int16_t)iy, type, r[1]};
    }
}

int forEachPoi(const uint8_t* b, size_t len, PoiVisitor fn, void* ctx) {
    if (!poiFileValid(b, len)) return 0;
    const uint8_t recSize = b[5];
    const uint16_t count = rd<uint16_t>(b + 6);
    const double lat0 = rd<double>(b + 16), lon0 = rd<double>(b + 24);
    const double kx = rd<float>(b + 32), ky = rd<float>(b + 36);
    const uint8_t* r = b + POI_HEADER_LEN;
    for (uint16_t i = 0; i < count; ++i, r += recSize)
        fn(ctx, r[0], r[1], lat0 + rd<int16_t>(r + 4) / ky, lon0 + rd<int16_t>(r + 2) / kx);
    return count;
}

void geoToScreen(double lat, double lon, double centerLat, double centerLon,
                 float metersPerPixel, int centerX, int centerY,
                 float rotateDeg, int16_t& sx, int16_t& sy) {
    double kxl = 111320.0 * cos(centerLat * M_PI / 180.0);
    float x = centerX + (float)((lon - centerLon) * kxl / metersPerPixel);
    float y = centerY - (float)((lat - centerLat) * 110540.0 / metersPerPixel);
    if (rotateDeg != 0) {
        float rc = cosf(rotateDeg * (float)M_PI / 180.0f);
        float rs = sinf(rotateDeg * (float)M_PI / 180.0f);
        float dx = x - centerX, dy = y - centerY;
        x = centerX + dx * rc - dy * rs;
        y = centerY + dx * rs + dy * rc;
    }
    if (x < -20000) x = -20000;
    if (x > 20000) x = 20000;
    if (y < -20000) y = -20000;
    if (y > 20000) y = 20000;
    sx = (int16_t)lroundf(x);
    sy = (int16_t)lroundf(y);
}

}  // namespace map_tiles
