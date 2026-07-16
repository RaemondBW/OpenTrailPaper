#pragma once

#include <cstddef>
#include <cstdint>

#include "map_view.h"

// Manages vector maps on the SD card plus the embedded default map. Two kinds
// of user map coexist:
//   * H3 tiles under /maps/tiles/<h3id>.ebm — small per-cell maps streamed
//     from the phone one at a time. Indexed by bbox in RAM; loaded on demand
//     into a small PSRAM LRU cache while rendering, so resident memory stays
//     bounded no matter how much of the world is on the card.
//   * Whole maps under /maps/*.ebm — the older single-blob format, kept as a
//     fallback and loaded entirely into PSRAM when it covers the position.
// The embedded default map is the final fallback.

namespace map_store {

// Load tiles index + the best whole map for a starting position (or the
// embedded default). Call after the SD card is mounted.
void begin(double lat, double lon);

// If the currently loaded whole map doesn't cover (lat, lon), load the
// downloaded one that does. Answered from the in-RAM bounds index, so outside
// the downloaded area (where there is nothing to load) this costs no SD access
// at all — it runs every second, on the bus the recorder writes the FIT to.
void ensureForPosition(double lat, double lon);

// Re-read the tile and whole-map indexes from the card. Call after something
// other than saveTile/saveAndActivate could have changed /maps — i.e. when a
// host computer releases the SD after mounting it over USB.
void rescanCard();

// Render the map around (lat, lon) into `out`: projects every H3 tile that
// overlaps the viewport (loading them from SD on demand), falling back to the
// whole-map / embedded blob where no tiles cover. Replaces a direct
// map_tiles::project() call.
void renderInto(double lat, double lon, float metersPerPixel, int centerX,
                int centerY, float rotateDeg, MapScreenData& out);

// Save a freshly received whole .ebm to /maps/<name>.ebm and make it active.
// Returns false on SD/parse error.
bool saveAndActivate(const char* name, const uint8_t* data, size_t len);

// Save a single received H3 tile to /maps/tiles/<id>.ebm and refresh the
// index so it renders immediately. Returns false on SD/parse error.
bool saveTile(const char* id, const uint8_t* data, size_t len);

// Whether tile <id> (H3 id without extension) is already on the card.
bool hasTile(const char* id);

// Number of H3 tiles currently indexed.
int tileCount();

// Whether any map (a downloaded tile, a whole map, or the embedded default)
// actually covers (lat, lon). False => the map screen has nothing to draw here
// and should show a "download this area" prompt.
bool coversPosition(double lat, double lon);

// Terrain elevation (metres) at (lat, lon), interpolated from the DEM grid
// baked into the covering tile (ELV1 block). NAN if no tile/elevation covers
// the point. Call from the UI task only (shares the tile LRU cache).
float elevationAt(double lat, double lon);

// Copy up to maxOut tile ids (H3 ids, no extension) into out for the app's
// dedup check. Returns the count written.
int listTileIds(char out[][24], int maxOut);

// Free bytes on the SD card (for the app to show remaining space).
uint32_t sdFreeKB();

// Coverage bounds of a stored map, for the app's "already downloaded" overlay.
struct MapBounds { double s, w, n, e; bool builtin; };

// Fill `out` with the embedded default + each downloaded /maps/*.ebm. Returns
// the count. Reads only each file's 36-byte header.
int listMaps(MapBounds* out, int maxOut);

}
