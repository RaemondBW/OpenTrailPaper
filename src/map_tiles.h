#pragma once

// Reader/projector for .ebm map binaries (see tools/maps/build_map.py).
// The blob is memory-mapped flash on device (board_build.embed_files)
// and a file read into RAM on the host. North-up view: given a center
// lat/lon and zoom, fills MapScreenData with screen-space polylines.

#include <cstddef>
#include <cstdint>

#include "map_view.h"

namespace map_tiles {

bool load(const uint8_t* data, size_t len);
bool loaded();

// EBM2 structure. A tile's header names its grid, and the grid sizes the
// sub-tile index; nothing else ties either to the length of the file, so every
// reader has to check them against it before it reads — a tile already on a
// card may have been stored by firmware that never looked.
// Where the sub-tile index ends: 0 when the header is not usable (not EBM2, a
// grid of no cells, a cell size that is not a small positive number, or an
// index that does not fit in the blob).
size_t tileIndexEnd(const uint8_t* blob, size_t len);
// Where the road data ends and the sections after it (ELV1, WTR2, ...) begin:
// past the index and every entry in it that lies inside the blob. 0 as above.
size_t tileDataEnd(const uint8_t* blob, size_t len);
// The whole structure: header and index as above, and every index entry,
// polyline record and section inside the blob. What a tile must be to be
// stored (saveTile, saveAndActivate). Readers do not need it — they skip
// whatever does not fit — but the phone deserves to hear that a tile is bad.
bool tileValid(const uint8_t* blob, size_t len);

// Projects features around (lat, lon) into `out` (features/featureCount
// only — route and rider fields are the caller's). Coordinates are
// screen px with the position at (centerX, centerY). Internal scratch
// buffers are reused; the result is valid until the next call.
// rotateDeg spins the world around the center (track-up = -heading).
void project(double lat, double lon, float metersPerPixel, int centerX,
             int centerY, float rotateDeg, MapScreenData& out);

// Multi-tile projection: the map_store layer renders several small tile
// blobs into one frame. beginProject resets the shared scratch cursors,
// projectBlobInto appends one blob's features, endProject publishes the
// count. Each blob carries its own EBM1 grid header.
void beginProject(MapScreenData& out);
void projectBlobInto(const uint8_t* blob, size_t blobLen, double lat,
                     double lon, float metersPerPixel, int centerX,
                     int centerY, float rotateDeg);
void endProject(MapScreenData& out);

// Diagnostics: polys accumulated so far this frame, and the per-class breakdown.
int projectedPolyCount();
void projectedClassCounts(int out[7]);

// What this frame had to throw away. A frame that silently drops half its
// geometry looks exactly like a frame with nothing to draw, which is what made
// the partly-drawn map hard to read as a bug; these counters name the limit
// that bit. Reset by beginProject().
struct MapProjectStats {
    int usedPoints;         // road/path scratch points in use
    int usedPolys;
    int usedWaterPoints, usedParkPoints;
    int roadsDropped;       // hit MAX_POLYS / MAX_POINTS — geometry lost
    int waterDropped;       // hit MAX_WATER_POLYS / MAX_WATER_POINTS
    int parksDropped;       // hit MAX_PARK_POLYS / MAX_PARK_POINTS
    int blobsTruncated;     // blobs abandoned mid-parse because scratch filled
    int roadsOffscreen;     // rejected: no kept vertex inside the viewport
    int waterOffscreen, parksOffscreen;
    int usedPois, poisDropped;  // cycling POIs kept / lost to MAX_POIS
    // The scratch capacities these are measured against, so a tool reporting
    // headroom reads the real numbers instead of a copy that goes stale.
    int capPoints, capPolys;
    int capWaterPoints, capWaterPolys;
    int capParkPoints, capParkPolys;
    int capPois;
};
MapProjectStats projectStats();

// Cycling POIs live in a .poi file next to each tile (format in
// map_tiles.cpp, writer in docs/mapgen.js buildPoi), not inside the .ebm.
constexpr size_t POI_HEADER_LEN = 40;
// Header magic/version/length check. Also how saveTile tells a .poi apart.
bool poiFileValid(const uint8_t* poi, size_t len);

// Append the POIs of one .poi file that this zoom shows to the frame's POI
// scratch (between beginProject and endProject, like projectBlobInto).
void projectPoisInto(const uint8_t* poi, size_t len, double lat, double lon,
                     float metersPerPixel, int centerX, int centerY,
                     float rotateDeg);

// Visit every POI in a .poi file, at any zoom, as lat/lon. Returns the count
// (0 for an invalid file).
using PoiVisitor = void (*)(void* ctx, uint8_t type, uint8_t flags, double lat,
                            double lon);
int forEachPoi(const uint8_t* poi, size_t len, PoiVisitor fn, void* ctx);

// Standalone geo -> screen projection around a center point (works
// without a loaded map; used for the route overlay).
void geoToScreen(double lat, double lon, double centerLat, double centerLon,
                 float metersPerPixel, int centerX, int centerY,
                 float rotateDeg, int16_t& sx, int16_t& sy);

}
