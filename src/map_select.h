#pragma once

// Which stored map tiles a frame needs. Pure geometry over the in-RAM tile
// bbox index — no SD, no Arduino — so tools/map_test can exercise the exact
// selection the device runs rather than a copy of it that drifts.

#include <cmath>

// Hard cap on the tile index (map_store's g_tiles).
constexpr int MAP_MAX_TILES = 512;

// Tiles one frame may project, and the number of slots in map_store's LRU.
//
// This has to cover the WIDEST view the device offers, or the map simply stops
// having ground on it out at the edges. Measured against real H3 res-6 coverage
// (~5.6 km across): 23 tiles at the widest north-up view, and 38 track-up,
// whose selection square has to contain the rotated viewport at any heading.
// The old budget was 32 — enough for north-up, six short of track-up, so the
// corners of the max zoom-out went blank exactly when the rider wanted the
// overview. 64 leaves headroom for a denser tiling without another rebuild.
//
// It is NOT a memory bound: the LRU is bounded by bytes as well as slots, and
// nothing holds more than one tile's bytes at a time while projecting.
constexpr int MAP_TILE_BUDGET = 64;

struct MapTileBox { double s, w, n, e; };

// Screen half-extents (px) of the tile-selection rectangle. The map draws under
// the status bar and footer, so those covered bands need no tiles; a ~410 px
// half-height covers what is actually shown. Track-up rotates the world about
// the rider, so a square that contains the rotated viewport is used instead.
inline void mapViewHalfExtentPx(float rotateDeg, float& hx, float& hy) {
    if (rotateDeg != 0.0f) { hx = 480.0f; hy = 480.0f; return; }
    hx = 290.0f;
    hy = 410.0f;
}

// Indices of the tiles overlapping the viewport around (lat, lon), NEAREST
// FIRST, written to `out` (at most maxOut). Returns how many were written;
// `*overlapping`, when given, receives how many overlapped in total — the two
// differ exactly when the frame cannot draw everything it needs.
//
// `boxAt(i)` returns tile i's bbox, so the caller's own index is read in place
// rather than copied into a second array (16 KB of internal RAM on the device).
//
// Nearest-first is not cosmetic: when the budget does bite, what survives has
// to be the ground the rider is on, and the hole has to be at the edges.
template <typename BoxAt>
int mapSelectTiles(int n, BoxAt boxAt, double lat, double lon,
                   float metersPerPixel, float rotateDeg, int maxOut, int* out,
                   int* overlapping = nullptr) {
    const double kx = 111320.0 * cos(lat * M_PI / 180.0);
    const double ky = 110540.0;
    float hx, hy;
    mapViewHalfExtentPx(rotateDeg, hx, hy);
    const double dLat = (hy * metersPerPixel) / ky;
    const double dLon = (hx * metersPerPixel) / (kx > 1 ? kx : 1);
    const double vs = lat - dLat, vn = lat + dLat;
    const double vw = lon - dLon, ve = lon + dLon;

    // Kept sorted by distance, capped at maxOut — so this needs room for the
    // BUDGET, not for the whole index, and the far tiles fall off the end as
    // nearer ones arrive.
    double best[MAP_TILE_BUDGET];
    int kept = 0;
    if (maxOut > MAP_TILE_BUDGET) maxOut = MAP_TILE_BUDGET;
    int nc = 0;

    for (int i = 0; i < n; ++i) {
        const MapTileBox t = boxAt(i);
        if (t.e < vw || t.w > ve || t.n < vs || t.s > vn) continue;
        nc++;
        // Distance from the rider to the NEAREST POINT of the tile, not to its
        // centre. Centre distance ranks the tile the rider is standing in below
        // a smaller neighbour once the viewport is wide, which is how a budget
        // cut used to drop ground from under the rider first.
        const double cx = lon < t.w ? t.w : (lon > t.e ? t.e : lon);
        const double cy = lat < t.s ? t.s : (lat > t.n ? t.n : lat);
        const double dx = (cx - lon) * kx, dy = (cy - lat) * ky;
        const double d2 = dx * dx + dy * dy;

        if (kept == maxOut && d2 >= best[kept - 1]) continue;
        int at = kept < maxOut ? kept : maxOut - 1;
        while (at > 0 && best[at - 1] > d2) {
            best[at] = best[at - 1];
            out[at] = out[at - 1];
            --at;
        }
        best[at] = d2;
        out[at] = i;
        if (kept < maxOut) ++kept;
    }
    if (overlapping) *overlapping = nc;
    return kept;
}
