#pragma once

// Which stored map tiles a frame needs. Pure geometry over the in-RAM tile
// bbox index — no SD, no Arduino — so tools/map_test can exercise the exact
// selection the device runs rather than a copy of it that drifts.

#include <cmath>

extern "C" {
#include "h3shim.h"
}

// Tiles one frame may project, and the number of slots in map_store's LRU.
//
// This has to cover the WIDEST view the device offers, or the map simply stops
// having ground on it out at the edges. Measured against real H3 res-6 coverage
// (~5.6 km across): 23 tiles at the widest north-up view, and 38 track-up,
// whose selection square has to contain the rotated viewport at any heading.
// 64 leaves headroom for a denser tiling without another rebuild.
//
// It is NOT a memory bound: the LRU is bounded by bytes as well as slots, and
// nothing holds more than one tile's bytes at a time while projecting.
constexpr int MAP_TILE_BUDGET = 64;

// Working room for the H3 disk before it is trimmed to the budget. A wide
// track-up view covers ~38 cells; this leaves the arithmetic somewhere to put
// the ones that turn out to be too far away.
constexpr int MAP_CELL_SCRATCH = 256;

// Screen half-extents (px) of the tile-selection rectangle. The map draws under
// the status bar and footer, so those covered bands need no tiles; a ~410 px
// half-height covers what is actually shown. Track-up rotates the world about
// the rider, so a square that contains the rotated viewport is used instead.
inline void mapViewHalfExtentPx(float rotateDeg, float& hx, float& hy) {
    if (rotateDeg != 0.0f) { hx = 480.0f; hy = 480.0f; return; }
    hx = 290.0f;
    hy = 410.0f;
}

// The H3 cells covering the viewport around (lat, lon), NEAREST FIRST, written
// to `out` (at most maxOut). Returns how many were written; `*overlapping`, when
// given, receives how many the viewport touched in total.
//
// COMPUTED, not looked up. A tile's filename is its H3 cell id, so the set of
// tiles a frame needs falls out of the geometry — there is no index to build,
// nothing to scan at boot, and no cap on how much of the world the card may
// hold. Cells with no tile downloaded simply fail to open.
//
// Nearest-first is not cosmetic: when the budget bites, what survives has to be
// the ground the rider is on, and the hole has to be at the edges.
inline int mapSelectCells(double lat, double lon, float metersPerPixel,
                          float rotateDeg, int maxOut, uint64_t* out,
                          int* overlapping = nullptr) {
    const double kx = 111320.0 * cos(lat * M_PI / 180.0);
    const double ky = 110540.0;
    float hx, hy;
    mapViewHalfExtentPx(rotateDeg, hx, hy);
    const double dLat = (hy * metersPerPixel) / ky;
    const double dLon = (hx * metersPerPixel) / (kx > 1 ? kx : 1);

    static uint64_t cells[MAP_CELL_SCRATCH];
    int nc = h3_covering_cells(lat - dLat, lon - dLon, lat + dLat, lon + dLon,
                               cells, MAP_CELL_SCRATCH);
    if (overlapping) *overlapping = nc;
    if (maxOut > MAP_TILE_BUDGET) maxOut = MAP_TILE_BUDGET;

    // Insertion sort into a fixed nearest-first list, so this needs room for the
    // BUDGET rather than for every cell the disk produced.
    double best[MAP_TILE_BUDGET];
    int kept = 0;
    for (int i = 0; i < nc; ++i) {
        double s, w, n, e;
        h3_cell_bbox(cells[i], &s, &w, &n, &e);
        // Distance to the NEAREST POINT of the cell, not to its centre: centre
        // distance ranks the cell the rider is standing in below a neighbour
        // once the viewport is wide.
        const double cx = lon < w ? w : (lon > e ? e : lon);
        const double cy = lat < s ? s : (lat > n ? n : lat);
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
        out[at] = cells[i];
        if (kept < maxOut) ++kept;
    }
    return kept;
}
