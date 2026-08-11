#include "h3shim.h"

#include <math.h>
#include <stdlib.h>

#include "h3api.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double toRad(double d) { return d * M_PI / 180.0; }
static double toDeg(double r) { return r * 180.0 / M_PI; }

void h3_cell_bbox(uint64_t cell, double *s, double *w, double *n, double *e) {
    CellBoundary b;
    cellToBoundary((H3Index)cell, &b);
    double ss = 90, ww = 180, nn = -90, ee = -180;
    for (int i = 0; i < b.numVerts; i++) {
        double la = toDeg(b.verts[i].lat), lo = toDeg(b.verts[i].lng);
        if (la < ss) ss = la;
        if (la > nn) nn = la;
        if (lo < ww) ww = lo;
        if (lo > ee) ee = lo;
    }
    *s = ss; *w = ww; *n = nn; *e = ee;
}

int h3_cell_boundary(uint64_t cell, double *latlng, int cap) {
    CellBoundary b;
    cellToBoundary((H3Index)cell, &b);
    int n = 0;
    for (int i = 0; i < b.numVerts && n < cap; i++) {
        latlng[n * 2] = toDeg(b.verts[i].lat);
        latlng[n * 2 + 1] = toDeg(b.verts[i].lng);
        n++;
    }
    return n;
}

int h3_covering_cells(double bs, double bw, double bn, double be,
                      uint64_t *out, int cap) {
    double clat = (bs + bn) / 2.0, clon = (bw + be) / 2.0;
    LatLng cg = { toRad(clat), toRad(clon) };
    H3Index center = 0;
    if (latLngToCell(&cg, 6, &center) || center == 0) return 0;

    // Ring radius (in cells) big enough to reach the box corners. Adjacent
    // res-6 cell centers are ~5 km apart.
    double dLatKm = (bn - bs) / 2.0 * 110.54;
    double dLonKm = (be - bw) / 2.0 * 111.32 * cos(toRad(clat));
    double halfDiagKm = sqrt(dLatKm * dLatKm + dLonKm * dLonKm);
    int k = (int)ceil(halfDiagKm / 5.0) + 1;
    if (k < 1) k = 1;
    if (k > 200) k = 200;   // safety cap (~1000 km box)

    int64_t maxN = 0;
    if (maxGridDiskSize(k, &maxN) || maxN <= 0) return 0;
    H3Index *disk = (H3Index *)calloc((size_t)maxN, sizeof(H3Index));
    if (!disk) return 0;
    gridDisk(center, k, disk);

    int cnt = 0;
    for (int64_t i = 0; i < maxN && cnt < cap; i++) {
        if (!disk[i]) continue;
        double s, w, n, e;
        h3_cell_bbox(disk[i], &s, &w, &n, &e);
        if (e < bw || w > be || n < bs || s > bn) continue;  // no overlap
        out[cnt++] = disk[i];
    }
    free(disk);
    return cnt;
}

void h3_cell_id(uint64_t cell, char *buf, int cap) {
    h3ToString((H3Index)cell, buf, (size_t)cap);
}

uint64_t h3_from_id(const char *s) {
    H3Index c = 0;
    if (stringToH3(s, &c)) return 0;
    return (uint64_t)c;
}

uint64_t h3_cell_at(double lat, double lng) {
    LatLng g = { toRad(lat), toRad(lng) };
    H3Index c = 0;
    if (latLngToCell(&g, 6, &c)) return 0;
    return (uint64_t)c;
}
