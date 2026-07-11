#include "map_tiles.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

namespace {

// Scratch capacity: worst case is dense downtown at wide zoom.
constexpr int MAX_POINTS = 60000;
constexpr int MAX_POLYS = 8000;

const uint8_t* blob = nullptr;
size_t blobLen = 0;

double gridLat0, gridLon0, tileDeg;
int32_t gridNx, gridNy;
const uint8_t* indexBase = nullptr;
double kx, ky;  // meters per degree lon/lat

int16_t* pts = nullptr;
MapPolyline* polys = nullptr;

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

}  // namespace

namespace map_tiles {

bool load(const uint8_t* data, size_t len) {
    if (len < 36 || memcmp(data, "EBM1", 4) != 0) return false;
    blob = data;
    blobLen = len;
    gridLat0 = rd<double>(data + 4);
    gridLon0 = rd<double>(data + 12);
    tileDeg = rd<double>(data + 20);
    gridNx = rd<int32_t>(data + 28);
    gridNy = rd<int32_t>(data + 32);
    indexBase = data + 36;

    double midLat = gridLat0 + tileDeg * gridNy / 2.0;
    kx = 111320.0 * cos(midLat * M_PI / 180.0);
    ky = 110540.0;

    if (!pts) {
        pts = (int16_t*)bigAlloc(MAX_POINTS * 2 * sizeof(int16_t));
        polys = (MapPolyline*)bigAlloc(MAX_POLYS * sizeof(MapPolyline));
    }
    return pts && polys;
}

bool loaded() { return blob != nullptr && pts != nullptr; }

void project(double lat, double lon, float metersPerPixel, int centerX,
             int centerY, float rotateDeg, MapScreenData& out) {
    out.features = polys;
    out.featureCount = 0;
    if (!loaded()) return;

    float rc = 1, rs = 0;
    if (rotateDeg != 0) {
        rc = cosf(rotateDeg * (float)M_PI / 180.0f);
        rs = sinf(rotateDeg * (float)M_PI / 180.0f);
    }

    // Viewport extent in meters around the center (portrait 540x960 max)
    const float halfWm = 300 * metersPerPixel;
    const float halfHm = 520 * metersPerPixel;

    // Position in grid meters
    double px = (lon - gridLon0) * kx;
    double py = (lat - gridLat0) * ky;

    double tileWm = tileDeg * kx, tileHm = tileDeg * ky;
    int tx0 = (int)floor((px - halfWm) / tileWm);
    int tx1 = (int)floor((px + halfWm) / tileWm);
    int ty0 = (int)floor((py - halfHm) / tileHm);
    int ty1 = (int)floor((py + halfHm) / tileHm);

    int usedPts = 0, usedPolys = 0;

    for (int ty = ty0; ty <= ty1; ++ty) {
        for (int tx = tx0; tx <= tx1; ++tx) {
            if (tx < 0 || tx >= gridNx || ty < 0 || ty >= gridNy) continue;
            uint32_t off = rd<uint32_t>(indexBase + (ty * gridNx + tx) * 8);
            uint32_t len = rd<uint32_t>(indexBase + (ty * gridNx + tx) * 8 + 4);
            if (!off || off + len > blobLen) continue;

            // Tile origin relative to position, in screen px
            float originX = centerX + (float)((tx * tileWm - px) / metersPerPixel);
            float originY = centerY - (float)((ty * tileHm - py) / metersPerPixel);

            const uint8_t* p = blob + off;
            const uint8_t* end = p + len;
            uint16_t count = rd<uint16_t>(p);
            p += 2;

            for (uint16_t i = 0; i < count && p + 3 <= end; ++i) {
                uint8_t cls = *p;
                uint16_t n = rd<uint16_t>(p + 1);
                p += 3;
                if (p + n * 4 > end) return;

                // Zoomed out: drop footpaths to keep draw time bounded.
                if (metersPerPixel >= 6.0f && cls == MAP_PATH) {
                    p += n * 4;
                    continue;
                }

                if (usedPolys >= MAX_POLYS || usedPts + n > MAX_POINTS) return;

                // Project + viewport reject in one pass
                int16_t* dst = pts + usedPts * 2;
                bool anyVisible = false;
                for (uint16_t j = 0; j < n; ++j) {
                    int16_t mx = rd<int16_t>(p + j * 4);
                    int16_t my = rd<int16_t>(p + j * 4 + 2);
                    float sx = originX + mx / metersPerPixel;
                    float sy = originY - my / metersPerPixel;
                    if (rotateDeg != 0) {
                        float dx = sx - centerX, dy = sy - centerY;
                        sx = centerX + dx * rc - dy * rs;
                        sy = centerY + dx * rs + dy * rc;
                    }
                    if (sx > -50 && sx < 590 && sy > -50 && sy < 1010) {
                        anyVisible = true;
                    }
                    // clamp to int16 to be safe at deep zoom-out
                    if (sx < -20000) sx = -20000;
                    if (sx > 20000) sx = 20000;
                    if (sy < -20000) sy = -20000;
                    if (sy > 20000) sy = 20000;
                    dst[j * 2] = (int16_t)lroundf(sx);
                    dst[j * 2 + 1] = (int16_t)lroundf(sy);
                }
                p += n * 4;

                if (!anyVisible) continue;
                polys[usedPolys].cls = (MapFeatureClass)cls;
                polys[usedPolys].pts = dst;
                polys[usedPolys].pointCount = n;
                usedPolys++;
                usedPts += n;
            }
        }
    }

    out.featureCount = usedPolys;
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
