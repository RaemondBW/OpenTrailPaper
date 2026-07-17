#include "map_store.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <cmath>
#include <cstring>

#include "map_tiles.h"
#include "sd_bus.h"
#include "usb_storage.h"
#include "diag.h"

// No embedded fallback map — the device only shows maps that were explicitly
// downloaded to the SD card. Where nothing covers the position the map screen
// shows "NO MAP HERE".

namespace {

constexpr char MAP_DIR[] = "/maps";
constexpr char TILE_DIR[] = "/maps/tiles";

uint8_t* activeBuf = nullptr;     // PSRAM copy of the active whole SD map (or null)
size_t activeLen = 0;

// The active whole-map blob rendered as a base where no per-position tiles
// cover us (null when no whole map is loaded).
const uint8_t* primaryBlob = nullptr;
size_t primaryLen = 0;

// Bounds of the currently loaded whole map, so we can tell when to switch.
double loadedS = 0, loadedW = 0, loadedN = 0, loadedE = 0;
bool haveBounds = false;

// Index of the whole maps on the card (name + bbox), so "which whole map covers
// this position?" is a RAM lookup rather than a scan of the card. Mirrors the
// H3 tile index below. Whole maps only appear/disappear when one is downloaded
// or a host computer edits the card, so this is rebuilt at those points instead
// of being re-derived from SD on every position update.
constexpr int MAX_MAPS = 32;
struct MapMeta { char name[56]; double s, w, n, e; };
MapMeta g_maps[MAX_MAPS];
int g_mapCount = 0;

// --- H3 tile layer -------------------------------------------------------
// Each downloaded H3 cell is its own small .ebm under /maps/tiles, named by
// its H3 id. We keep a lightweight in-memory index of (name, bbox) and load
// the actual tile bytes on demand into a small PSRAM LRU cache. This caps
// resident map memory regardless of how much of the world is on the card.
constexpr int MAX_TILES = 512;
struct TileMeta { char name[28]; double s, w, n, e; };
TileMeta g_tiles[MAX_TILES];
int g_tileCount = 0;

constexpr int CACHE_N = 20;   // enough resident tiles for a wide zoom-out view
struct CachedTile { int idx; uint8_t* buf; size_t len; uint32_t stamp; };
CachedTile g_cache[CACHE_N] = {};
uint32_t g_clock = 0;

template <typename T>
T rd(const uint8_t* p) { T v; memcpy(&v, p, sizeof(T)); return v; }

// Read the EBM2 header (36 bytes) and fill s/w/n/e. Returns false if not EBM2.
bool headerBounds(const uint8_t* h, double& s, double& w, double& n, double& e) {
    if (memcmp(h, "EBM2", 4) != 0) return false;
    double lat0 = rd<double>(h + 4);
    double lon0 = rd<double>(h + 12);
    double td = rd<double>(h + 20);
    int32_t nx = rd<int32_t>(h + 28);
    int32_t ny = rd<int32_t>(h + 32);
    s = lat0; w = lon0;
    n = lat0 + td * ny;
    e = lon0 + td * nx;
    return true;
}

bool boundsCover(double lat, double lon) {
    return haveBounds && lat >= loadedS && lat <= loadedN &&
           lon >= loadedW && lon <= loadedE;
}

// No whole map covers us (or none is downloaded): clear the primary base so
// the renderer draws only tiles (if any) and the map screen reports no coverage.
void clearPrimary() {
    primaryBlob = nullptr;
    primaryLen = 0;
    haveBounds = false;
}

// Read a whole .ebm file into a fresh PSRAM buffer and make it the active map.
bool loadFile(const char* path) {
    sdLock();
    File f = SD.open(path, FILE_READ);
    if (!f) { sdUnlock(); return false; }
    size_t len = f.size();
    if (len < 36) { f.close(); sdUnlock(); return false; }
    uint8_t* buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!buf) { f.close(); sdUnlock(); return false; }
    size_t got = f.read(buf, len);
    f.close();
    sdUnlock();
    if (got != len || !map_tiles::load(buf, len)) {
        heap_caps_free(buf);
        return false;
    }
    if (activeBuf) heap_caps_free(activeBuf);   // free the previous SD map
    activeBuf = buf;
    activeLen = len;
    primaryBlob = buf;
    primaryLen = len;
    haveBounds = headerBounds(buf, loadedS, loadedW, loadedN, loadedE);
    diag::log("map: loaded %s (%u KB)", path, (unsigned)(len / 1024));
    return true;
}

// (Re)build the in-memory whole-map index from the /maps headers.
void scanMaps() {
    g_mapCount = 0;
    sdLock();
    if (!SD.exists(MAP_DIR)) SD.mkdir(MAP_DIR);
    File dir = SD.open(MAP_DIR);
    if (dir) {
        for (File e = dir.openNextFile(); e && g_mapCount < MAX_MAPS;
             e = dir.openNextFile()) {
            if (!e.isDirectory()) {
                const char* nm = e.name();
                const char* base = strrchr(nm, '/');
                base = base ? base + 1 : nm;
                if (strstr(base, ".ebm")) {
                    uint8_t h[36];
                    double s, w, n, ee;
                    if (e.read(h, 36) == 36 && headerBounds(h, s, w, n, ee)) {
                        MapMeta& m = g_maps[g_mapCount++];
                        strncpy(m.name, base, sizeof(m.name) - 1);
                        m.name[sizeof(m.name) - 1] = 0;
                        m.s = s; m.w = w; m.n = n; m.e = ee;
                    }
                }
            }
            e.close();
        }
        dir.close();
    }
    sdUnlock();
    diag::log("map: %d whole maps indexed", g_mapCount);
}

// Find an indexed whole map whose bounds contain (lat, lon); load if found.
// A RAM lookup — no SD work unless there's actually a map to load.
bool loadCovering(double lat, double lon) {
    for (int i = 0; i < g_mapCount; ++i) {
        const MapMeta& m = g_maps[i];
        if (lat < m.s || lat > m.n || lon < m.w || lon > m.e) continue;
        char path[80];
        snprintf(path, sizeof(path), "%s/%s", MAP_DIR, m.name);
        return loadFile(path);
    }
    return false;
}

// (Re)build the in-memory tile index from /maps/tiles headers. Invalidates
// the LRU cache since index positions may shift.
void scanTiles() {
    for (int i = 0; i < CACHE_N; ++i) {
        if (g_cache[i].buf) heap_caps_free(g_cache[i].buf);
        g_cache[i] = {};
        g_cache[i].idx = -1;
    }
    g_tileCount = 0;
    sdLock();
    if (!SD.exists(MAP_DIR)) SD.mkdir(MAP_DIR);
    if (!SD.exists(TILE_DIR)) SD.mkdir(TILE_DIR);
    File dir = SD.open(TILE_DIR);
    if (dir) {
        for (File f = dir.openNextFile(); f && g_tileCount < MAX_TILES;
             f = dir.openNextFile()) {
            if (!f.isDirectory()) {
                const char* nm = f.name();
                const char* base = strrchr(nm, '/');
                base = base ? base + 1 : nm;
                if (strstr(base, ".ebm")) {
                    uint8_t h[36];
                    double s, w, n, e;
                    if (f.read(h, 36) == 36 && headerBounds(h, s, w, n, e)) {
                        TileMeta& t = g_tiles[g_tileCount++];
                        strncpy(t.name, base, sizeof(t.name) - 1);
                        t.name[sizeof(t.name) - 1] = 0;
                        t.s = s; t.w = w; t.n = n; t.e = e;
                    }
                }
            }
            f.close();
        }
        dir.close();
    }
    sdUnlock();
    diag::log("map: %d tiles indexed", g_tileCount);
}

// Return the tile blob for index `idx`, loading it from SD into the LRU cache
// if needed. Returns null on read error, or if the USB host owns the SD and
// the tile isn't already cached (we don't touch SD while it's mounted).
const uint8_t* ensureTileLoaded(int idx, size_t& outLen) {
    for (int i = 0; i < CACHE_N; ++i) {
        if (g_cache[i].buf && g_cache[i].idx == idx) {
            g_cache[i].stamp = ++g_clock;
            outLen = g_cache[i].len;
            return g_cache[i].buf;
        }
    }
    if (usb_storage::hostActive()) { outLen = 0; return nullptr; }

    char path[80];
    snprintf(path, sizeof(path), "%s/%s", TILE_DIR, g_tiles[idx].name);
    sdLock();
    File f = SD.open(path, FILE_READ);
    size_t len = f ? f.size() : 0;
    uint8_t* buf = nullptr;
    if (f && len >= 36) {
        buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (buf && f.read(buf, len) != len) { heap_caps_free(buf); buf = nullptr; }
    }
    if (f) f.close();
    sdUnlock();
    if (!buf) { outLen = 0; return nullptr; }

    // Evict the least-recently-used (or a free) slot.
    CachedTile* victim = &g_cache[0];
    for (int i = 0; i < CACHE_N; ++i) {
        if (!g_cache[i].buf) { victim = &g_cache[i]; break; }
        if (g_cache[i].stamp < victim->stamp) victim = &g_cache[i];
    }
    if (victim->buf) heap_caps_free(victim->buf);
    victim->buf = buf; victim->len = len; victim->idx = idx;
    victim->stamp = ++g_clock;
    outLen = len;
    return buf;
}

// Interpolate elevation from a blob's ELV1 grid (appended after the tile data).
// Returns NAN if the blob has no elevation block covering (lat, lon).
float elevFromBlob(const uint8_t* b, size_t len, double lat, double lon) {
    if (len < 36 || memcmp(b, "EBM2", 4) != 0) return NAN;
    int32_t nx = rd<int32_t>(b + 28), ny = rd<int32_t>(b + 32);
    const uint8_t* index = b + 36;
    size_t maxEnd = 36 + (size_t)nx * ny * 8;      // end of the tile data
    for (int k = 0; k < nx * ny; ++k) {
        uint32_t off = rd<uint32_t>(index + (size_t)k * 8);
        uint32_t l = rd<uint32_t>(index + (size_t)k * 8 + 4);
        if (off && (size_t)off + l > maxEnd) maxEnd = (size_t)off + l;
    }
    if (maxEnd + 44 > len || memcmp(b + maxEnd, "ELV1", 4) != 0) return NAN;
    const uint8_t* p = b + maxEnd + 4;
    int32_t gw = rd<int32_t>(p), gh = rd<int32_t>(p + 4);
    double s = rd<double>(p + 8), w = rd<double>(p + 16);
    double n = rd<double>(p + 24), e = rd<double>(p + 32);
    const uint8_t* grid = p + 40;
    if (gw < 2 || gh < 2 || e <= w || n <= s) return NAN;
    if (maxEnd + 44 + (size_t)gw * gh * 2 > len) return NAN;

    double fx = (lon - w) / (e - w) * (gw - 1);
    double fy = (lat - s) / (n - s) * (gh - 1);
    if (fx < 0) fx = 0; if (fx > gw - 1) fx = gw - 1;
    if (fy < 0) fy = 0; if (fy > gh - 1) fy = gh - 1;
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = x0 + 1 < gw ? x0 + 1 : x0;
    int y1 = y0 + 1 < gh ? y0 + 1 : y0;
    float tx = (float)(fx - x0), ty = (float)(fy - y0);
    auto gv = [&](int xx, int yy) -> float {
        return (float)rd<int16_t>(grid + ((size_t)yy * gw + xx) * 2);
    };
    float top = gv(x0, y0) + (gv(x1, y0) - gv(x0, y0)) * tx;
    float bot = gv(x0, y1) + (gv(x1, y1) - gv(x0, y1)) * tx;
    return top + (bot - top) * ty;
}

}  // namespace

namespace map_store {

void begin(double lat, double lon) {
    sdLock();
    if (!SD.exists(MAP_DIR)) SD.mkdir(MAP_DIR);
    sdUnlock();
    scanTiles();
    scanMaps();
    if (!loadCovering(lat, lon)) clearPrimary();   // no fallback map
}

void rescanCard() {
    scanTiles();
    scanMaps();
}

// Called once per rendered frame (~1 Hz), including the whole time the rider is
// outside the downloaded area — so it must not touch the SD card unless there
// is actually a map to load. The recorder appends to the FIT on the same SPI
// bus under the same mutex once a second, and a ride must never be held up by
// map bookkeeping. Hence the RAM-resident bounds index.
void ensureForPosition(double lat, double lon) {
    if (boundsCover(lat, lon)) return;      // current whole map already covers us
    if (loadCovering(lat, lon)) return;     // switched to a better downloaded map
    clearPrimary();                         // nothing whole-map covers us now
}

void renderInto(double lat, double lon, float metersPerPixel, int centerX,
                int centerY, float rotateDeg, MapScreenData& out) {
    map_tiles::beginProject(out);

    // Viewport half-extents (portrait 540x960 max) as lat/lon deltas.
    double kx = 111320.0 * cos(lat * M_PI / 180.0);
    double ky = 110540.0;
    double dLat = (520.0 * metersPerPixel) / ky;
    double dLon = (300.0 * metersPerPixel) / (kx > 1 ? kx : 1);
    double vs = lat - dLat, vn = lat + dLat, vw = lon - dLon, ve = lon + dLon;

    int projected = 0;
    for (int i = 0; i < g_tileCount && projected < 40; ++i) {
        const TileMeta& t = g_tiles[i];
        if (t.e < vw || t.w > ve || t.n < vs || t.s > vn) continue;  // no overlap
        size_t len;
        const uint8_t* b = ensureTileLoaded(i, len);
        if (b) {
            map_tiles::projectBlobInto(b, len, lat, lon, metersPerPixel,
                                       centerX, centerY, rotateDeg);
            projected++;
        }
    }

    // Always draw the whole-map / embedded blob as a BASE layer underneath the
    // tiles. Tiles cover only the downloaded area; when zoomed out past them the
    // rest of the viewport would otherwise be blank. The base fills those gaps
    // where it has data (its viewport reject draws nothing outside its bounds).
    // Tiles are projected first so they win the polygon budget; where both have
    // the same roads they land on the same pixels (identical 3 m simplification),
    // so the overlap is invisible.
    if (primaryBlob) {
        map_tiles::projectBlobInto(primaryBlob, primaryLen, lat, lon,
                                   metersPerPixel, centerX, centerY, rotateDeg);
    }
    map_tiles::endProject(out);
}

bool saveAndActivate(const char* name, const uint8_t* data, size_t len) {
    if (len < 36 || memcmp(data, "EBM2", 4) != 0) {
        diag::log("map save rejected: not EBM2 (%u bytes)", (unsigned)len);
        return false;
    }
    char path[80];
    snprintf(path, sizeof(path), "%s/%.48s", MAP_DIR, name);
    if (!strstr(path, ".ebm")) strncat(path, ".ebm", sizeof(path) - strlen(path) - 1);

    sdLock();
    if (!SD.exists(MAP_DIR)) SD.mkdir(MAP_DIR);
    SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    bool ok = (bool)f;
    size_t wrote = 0;
    while (ok && wrote < len) {
        size_t chunk = len - wrote < 4096 ? len - wrote : 4096;
        if (f.write(data + wrote, chunk) != chunk) ok = false;
        else wrote += chunk;
    }
    if (f) f.close();
    if (!ok) SD.remove(path);
    sdUnlock();
    if (!ok) { diag::log("map save: write failed %s", path); return false; }
    diag::log("map saved: %s (%u KB)", path, (unsigned)(len / 1024));
    scanMaps();   // index the new map so ensureForPosition can find it later
    return loadFile(path);
}

bool saveTile(const char* id, const uint8_t* data, size_t len) {
    if (len < 36 || memcmp(data, "EBM2", 4) != 0) {
        diag::log("tile save rejected: not EBM2 (%u bytes)", (unsigned)len);
        return false;
    }
    char path[80];
    snprintf(path, sizeof(path), "%s/%.40s", TILE_DIR, id);
    if (!strstr(path, ".ebm")) strncat(path, ".ebm", sizeof(path) - strlen(path) - 1);

    sdLock();
    if (!SD.exists(MAP_DIR)) SD.mkdir(MAP_DIR);
    if (!SD.exists(TILE_DIR)) SD.mkdir(TILE_DIR);
    SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    bool ok = (bool)f;
    size_t wrote = 0;
    while (ok && wrote < len) {
        size_t chunk = len - wrote < 4096 ? len - wrote : 4096;
        if (f.write(data + wrote, chunk) != chunk) ok = false;
        else wrote += chunk;
    }
    if (f) f.close();
    if (!ok) SD.remove(path);
    sdUnlock();
    if (!ok) { diag::log("tile save: write failed %s", path); return false; }
    diag::log("tile saved: %s (%u KB)", path, (unsigned)(len / 1024));
    scanTiles();   // refresh index so the new tile renders immediately
    return true;
}

bool hasTile(const char* id) {
    for (int i = 0; i < g_tileCount; ++i) {
        // g_tiles[i].name is "<id>.ebm"; match the id prefix.
        if (strncmp(g_tiles[i].name, id, strlen(id)) == 0 &&
            g_tiles[i].name[strlen(id)] == '.') return true;
    }
    return false;
}

int tileCount() { return g_tileCount; }

bool coversPosition(double lat, double lon) {
    for (int i = 0; i < g_tileCount; ++i) {
        const TileMeta& t = g_tiles[i];
        if (lat >= t.s && lat <= t.n && lon >= t.w && lon <= t.e) return true;
    }
    return haveBounds && lat >= loadedS && lat <= loadedN &&
           lon >= loadedW && lon <= loadedE;
}

float elevationAt(double lat, double lon) {
    for (int i = 0; i < g_tileCount; ++i) {
        const TileMeta& t = g_tiles[i];
        if (lat < t.s || lat > t.n || lon < t.w || lon > t.e) continue;
        size_t len;
        const uint8_t* b = ensureTileLoaded(i, len);
        if (!b) continue;
        float ev = elevFromBlob(b, len, lat, lon);
        if (!isnan(ev)) return ev;
    }
    return NAN;
}

int listTileIds(char out[][24], int maxOut) {
    int n = 0;
    for (int i = 0; i < g_tileCount && n < maxOut; ++i) {
        strncpy(out[n], g_tiles[i].name, 23);
        out[n][23] = 0;
        char* dot = strstr(out[n], ".ebm");
        if (dot) *dot = 0;
        n++;
    }
    return n;
}

uint32_t sdFreeKB() {
    sdLock();
    uint32_t kb = (uint32_t)((SD.totalBytes() - SD.usedBytes()) / 1024);
    sdUnlock();
    return kb;
}

int listMaps(MapBounds* out, int maxOut) {
    int n = 0;
    sdLock();
    File dir = SD.open(MAP_DIR);
    if (dir) {
        for (File f = dir.openNextFile(); f && n < maxOut; f = dir.openNextFile()) {
            if (!f.isDirectory()) {
                const char* nm = f.name();
                const char* base = strrchr(nm, '/');
                base = base ? base + 1 : nm;
                if (strstr(base, ".ebm")) {
                    uint8_t h[36];
                    double s, w, nn, e;
                    if (f.read(h, 36) == 36 && headerBounds(h, s, w, nn, e)) {
                        out[n] = {s, w, nn, e, false};
                        n++;
                    }
                }
            }
            f.close();
        }
        dir.close();
    }
    sdUnlock();
    return n;
}

}  // namespace map_store
