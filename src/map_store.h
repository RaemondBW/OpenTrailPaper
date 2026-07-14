#pragma once

#include <cstddef>
#include <cstdint>

// Manages vector maps on the SD card (/maps/*.ebm) plus the embedded default
// map. User maps are downloaded from the phone; the store loads whichever one
// covers the current position, falling back to the embedded map. The active
// map blob lives in PSRAM so map_tiles' raw pointer into it stays valid.

namespace map_store {

// Load the best map for a starting position (or the embedded default). Call
// after the SD card is mounted.
void begin(double lat, double lon);

// If the currently loaded map doesn't cover (lat, lon), look for a downloaded
// map that does and load it. Cheap no-op when the current map still covers.
void ensureForPosition(double lat, double lon);

// Save a freshly received .ebm to /maps/<name>.ebm and make it active.
// Returns false on SD/parse error.
bool saveAndActivate(const char* name, const uint8_t* data, size_t len);

// Free bytes on the SD card (for the app to show remaining space).
uint32_t sdFreeKB();

// Coverage bounds of a stored map, for the app's "already downloaded" overlay.
struct MapBounds { double s, w, n, e; bool builtin; };

// Fill `out` with the embedded default + each downloaded /maps/*.ebm. Returns
// the count. Reads only each file's 36-byte header.
int listMaps(MapBounds* out, int maxOut);

}
