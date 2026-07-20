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

// Standalone geo -> screen projection around a center point (works
// without a loaded map; used for the route overlay).
void geoToScreen(double lat, double lon, double centerLat, double centerLon,
                 float metersPerPixel, int centerX, int centerY,
                 float rotateDeg, int16_t& sx, int16_t& sy);

}
