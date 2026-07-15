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

// If the currently loaded whole map doesn't cover (lat, lon), look for a
// downloaded one that does. No-op when tiles are what's covering us.
void ensureForPosition(double lat, double lon);

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
