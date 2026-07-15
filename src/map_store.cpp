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

// Embedded default map (board_build.embed_files = data/sf.ebm).
extern const uint8_t map_ebm_start[] asm("_binary_data_sf_ebm_start");
extern const uint8_t map_ebm_end[] asm("_binary_data_sf_ebm_end");

namespace {

constexpr char MAP_DIR[] = "/maps";
constexpr char TILE_DIR[] = "/maps/tiles";

uint8_t* activeBuf = nullptr;     // PSRAM copy of the active whole SD map (or null)
size_t activeLen = 0;
bool usingEmbedded = true;

// The whole-map/embedded blob rendered when no per-position tiles cover us.
const uint8_t* primaryBlob = nullptr;
size_t primaryLen = 0;

// Bounds of the currently loaded whole map, so we can tell when to switch.
double loadedS = 0, loadedW = 0, loadedN = 0, loadedE = 0;
bool haveBounds = false;

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

// Read the EBM1 header (36 bytes) and fill s/w/n/e. Returns false if not EBM1.
bool headerBounds(const uint8_t* h, double& s, double& w, double& n, double& e) {
    if (memcmp(h, "EBM1", 4) != 0) return false;
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

void loadEmbedded() {
    size_t len = map_ebm_end - map_ebm_start;
    if (map_tiles::load(map_ebm_start, len)) {
        usingEmbedded = true;
        primaryBlob = map_ebm_start;
        primaryLen = len;
        haveBounds = headerBounds(map_ebm_start, loadedS, loadedW, loadedN, loadedE);
        diag::log("map: embedded default (%u KB)", (unsigned)(len / 1024));
    }
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
    usingEmbedded = false;
    primaryBlob = buf;
    primaryLen = len;
    haveBounds = headerBounds(buf, loadedS, loadedW, loadedN, loadedE);
    diag::log("map: loaded %s (%u KB)", path, (unsigned)(len / 1024));
    return true;
}

// Find a downloaded whole map whose bounds contain (lat, lon); load if found.
bool loadCovering(double lat, double lon) {
    sdLock();
    File dir = SD.open(MAP_DIR);
    if (!dir) { sdUnlock(); return false; }
    char best[64] = "";
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
        if (!e.isDirectory()) {
            const char* nm = e.name();
            const char* base = strrchr(nm, '/');
            base = base ? base + 1 : nm;
            if (strstr(base, ".ebm")) {
                uint8_t h[36];
                if (e.read(h, 36) == 36) {
                    double s, w, n, ee;
                    if (headerBounds(h, s, w, n, ee) &&
                        lat >= s && lat <= n && lon >= w && lon <= ee) {
                        snprintf(best, sizeof(best), "%s/%s", MAP_DIR, base);
                    }
                }
            }
        }
        e.close();
        if (best[0]) break;
    }
    dir.close();
    sdUnlock();
    return best[0] ? loadFile(best) : false;
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

}  // namespace

namespace map_store {

void begin(double lat, double lon) {
    sdLock();
    if (!SD.exists(MAP_DIR)) SD.mkdir(MAP_DIR);
    sdUnlock();
    scanTiles();
    if (!loadCovering(lat, lon)) loadEmbedded();
}

void ensureForPosition(double lat, double lon) {
    if (boundsCover(lat, lon)) return;      // current whole map already covers us
    if (loadCovering(lat, lon)) return;     // switched to a better downloaded map
    if (!usingEmbedded && !haveBounds) loadEmbedded();
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

    // No H3 tiles cover this spot: fall back to the whole-map / embedded blob.
    if (projected == 0 && primaryBlob) {
        map_tiles::projectBlobInto(primaryBlob, primaryLen, lat, lon,
                                   metersPerPixel, centerX, centerY, rotateDeg);
    }
    map_tiles::endProject(out);
}

bool saveAndActivate(const char* name, const uint8_t* data, size_t len) {
    if (len < 36 || memcmp(data, "EBM1", 4) != 0) {
        diag::log("map save rejected: not EBM1 (%u bytes)", (unsigned)len);
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
    return loadFile(path);
}

bool saveTile(const char* id, const uint8_t* data, size_t len) {
    if (len < 36 || memcmp(data, "EBM1", 4) != 0) {
        diag::log("tile save rejected: not EBM1 (%u bytes)", (unsigned)len);
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
    // Embedded default first.
    if (n < maxOut) {
        double s, w, nn, e;
        if (headerBounds(map_ebm_start, s, w, nn, e)) {
            out[n] = {s, w, nn, e, true};
            n++;
        }
    }
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
