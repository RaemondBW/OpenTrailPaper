// Thin C shim over the vendored H3 library so Swift can work in plain scalars
// and avoid H3's fixed-array C structs (which import as awkward Swift tuples).
// All lat/lng here are in DEGREES.
#ifndef H3SHIM_H
#define H3SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// H3 res-6 cells covering the drawn box [s,w]-[n,e]: every hexagon that
// overlaps the box (found via a gridDisk around the center cell, then filtered
// by bbox intersection). Writes up to `cap` cell ids to `out`, returns count.
int h3_covering_cells(double s, double w, double n, double e,
                      uint64_t *out, int cap);

// Lat/lng bounding box (degrees) of one H3 cell's hexagon boundary.
void h3_cell_bbox(uint64_t cell, double *s, double *w, double *n, double *e);

// The cell's hexagon boundary as up to `cap` (lat,lng) degree pairs written to
// `latlng` (lat0,lng0,lat1,lng1,…). Returns the vertex count (usually 6).
int h3_cell_boundary(uint64_t cell, double *latlng, int cap);

// Lowercase hex id string of a cell (e.g. "86283082fffffff"). `buf` >= 17.
void h3_cell_id(uint64_t cell, char *buf, int cap);

// Parse a hex id string back to a cell (0 on error).
uint64_t h3_from_id(const char *s);

// The res-6 H3 cell containing (lat,lng) in degrees (0 on error). Used to
// hit-test which hexagon a map tap landed in.
uint64_t h3_cell_at(double lat, double lng);

#ifdef __cplusplus
}
#endif

#endif
