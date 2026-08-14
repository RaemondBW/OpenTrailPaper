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
};
MapProjectStats projectStats();

// Standalone geo -> screen projection around a center point (works
// without a loaded map; used for the route overlay).
void geoToScreen(double lat, double lon, double centerLat, double centerLon,
                 float metersPerPixel, int centerX, int centerY,
                 float rotateDeg, int16_t& sx, int16_t& sy);

}
