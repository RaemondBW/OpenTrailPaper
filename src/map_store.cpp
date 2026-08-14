#include "map_store.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <cmath>
#include <cstring>

#include "map_select.h"
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

// The tile index and the tile cache are shared by TWO tasks, and used to be
// shared with no lock at all:
//
//   UI task     — renderInto / elevationAt / coversPosition / ensureForPosition
//   BLE server  — saveTile / listTileIds / listMaps / saveAndActivate
//
// saveTile rebuilt the whole index, which starts by freeing every cached tile
// blob. Do that while the UI task is holding a pointer into that cache — or,
// worse, half way through its own eviction — and you get a use-after-free or a
// double free in PSRAM, which lands as a panic and a reset some indeterminate
// time later. It fires more often the more tiles are sent, because the rebuild
// ran once PER TILE, so a large transfer was hundreds of chances to hit it.
//
// This guards the RAM structures (the tile cache, the presence cache, g_maps
// active whole-map pointers). It is NOT the SD lock: it is always taken OUTSIDE
// sdLock, never the other way round, so the two can never deadlock.
// A function-local static, so the mutex exists before the first caller whoever
// that is — begin() runs on the UI task and the BLE server is already up by
// then, so there is no safe "create it in setup()" moment to rely on.
SemaphoreHandle_t mapMutex() {
    static SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
    return m;
}

void mapLock() {
    if (SemaphoreHandle_t m = mapMutex()) xSemaphoreTakeRecursive(m, portMAX_DELAY);
}
void mapUnlock() {
    if (SemaphoreHandle_t m = mapMutex()) xSemaphoreGiveRecursive(m);
}

// Scope guard so an early return can't leak the lock.
struct MapGuard {
    MapGuard() { mapLock(); }
    ~MapGuard() { mapUnlock(); }
};

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
// Each downloaded H3 cell is its own small .ebm under /maps/tiles, named by its
// H3 id, and loaded on demand into a small PSRAM LRU cache.
//
// THERE IS NO INDEX. A tile's filename IS its H3 cell id, and an H3 id encodes
// its own position on the globe — so "which tiles cover this viewport?" is
// arithmetic (see map_select.h), and "where is that tile?" is string formatting.
// Nothing has to be read off the card to find out.
//
// What that replaces: a boot-time scan that opened every .ebm on the card to
// read its 36-byte header into a bbox index. That cost ~37 ms a tile — 4.8 s at
// 129 tiles, growing with the card — held the SD lock throughout, blocked
// setup(), and capped coverage at however many entries the index had room for.
// It was also the longest stretch of a boot with no repaint, which is what made
// the panel wedge and the device boot-loop once a rider's card grew past it.
constexpr int TILE_PREFIX_LEN = 6;

// "<TILE_DIR>/<prefix>" for this id. Ids shorter than the prefix (or odd input)
// fall back to the flat directory rather than producing a stub folder.
void tileDirFor(const char* id, char* out, size_t len) {
    int n = 0;
    while (n < TILE_PREFIX_LEN && id[n] && id[n] != '.') ++n;
    if (n < TILE_PREFIX_LEN) { snprintf(out, len, "%s", TILE_DIR); return; }
    snprintf(out, len, "%s/%.*s", TILE_DIR, TILE_PREFIX_LEN, id);
}

// On-disk path for a cell: /maps/tiles/<first 6 of id>/<rest>.ebm. Mirrors
// saveTile(), the website generator and the phone app — all four have to agree
// on this or the device looks for files nobody wrote.
void tilePathForId(const char* bareId, char* out, size_t len) {
    char dir[80];
    tileDirFor(bareId, dir, sizeof(dir));
    const char* leaf = (strlen(dir) > strlen(TILE_DIR)) ? bareId + TILE_PREFIX_LEN
                                                        : bareId;
    snprintf(out, len, "%s/%s.ebm", dir, leaf);
}

void tilePathForCell(uint64_t cell, char* out, size_t len) {
    char id[24];
    h3_cell_id(cell, id, sizeof(id));
    tilePathForId(id, out, len);
}

// Does a tile exist for this cell? Answered from a small direct-mapped cache,
// because the alternative is asking the card the same question every frame —
// and the interesting answer is usually "no": a viewport at the edge of the
// downloaded area asks about plenty of cells nobody ever downloaded.
constexpr int PRESENCE_N = 256;
struct CellPresence { uint64_t cell; bool present; };
CellPresence g_presence[PRESENCE_N] = {};

void forgetPresence(uint64_t cell) {
    CellPresence& e = g_presence[(size_t)(cell % PRESENCE_N)];
    if (e.cell == cell) e = {};
}
void forgetAllPresence() {
    for (int i = 0; i < PRESENCE_N; ++i) g_presence[i] = {};
}

bool tileExists(uint64_t cell) {
    CellPresence& e = g_presence[(size_t)(cell % PRESENCE_N)];
    if (e.cell == cell) return e.present;
    char path[96];
    tilePathForCell(cell, path, sizeof(path));
    sdLock();
    bool got = SD.exists(path);
    sdUnlock();
    e.cell = cell;
    e.present = got;
    return got;
}



// The tile LRU. One slot per tile the widest frame can ask for (MAP_TILE_BUDGET
// — see map_select.h), so a working set that fits in the byte budget stays
// entirely cached and repeated zoom in/out costs no SD reads at all.
//
// Bounded by BYTES as well as slots, because a slot is not a fixed cost: a
// res-6 tile is ~50 KB of quiet suburb but over 200 KB of dense city, and 64
// dense slots would be more PSRAM than this device has spare. Past the byte
// budget the far tiles simply stream — the frame still draws every tile it
// selected, it just re-reads some of them next time. Correctness never depends
// on the cache, only speed: renderInto finishes with one tile's bytes before it
// asks for the next.
// One megabyte holds the working set of every zoom the rider actually spends
// time at — 2 to 16 m/px needs 2 to 15 tiles, 100 to 750 KB — and lets the
// widest track-up view (38 tiles, ~1.9 MB) stream the outer half from the card
// instead of reserving PSRAM all day for a view that is glanced at.
//
// It is reserved against OTA: staging a firmware image wants ~1.9 MB of
// CONTIGUOUS PSRAM in one allocation, and a full tile cache is exactly the kind
// of long-lived block that fragments that away. releaseCache() exists for it.
constexpr int CACHE_N = MAP_TILE_BUDGET;
constexpr size_t CACHE_BYTES = 1024 * 1024;
struct CachedTile { uint64_t cell; uint8_t* buf; size_t len; uint32_t stamp; };
CachedTile g_cache[CACHE_N] = {};
size_t g_cacheBytes = 0;
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

// Drop a cached blob for one cell (its file on the card just changed).
void invalidateCached(uint64_t cell) {
    for (int i = 0; i < CACHE_N; ++i) {
        if (g_cache[i].buf && g_cache[i].cell == cell) {
            heap_caps_free(g_cache[i].buf);
            g_cacheBytes -= g_cache[i].len;
            g_cache[i] = {};
        }
    }
}

// The blob for one H3 cell, read from the card into the LRU cache on demand.
// Null when no tile covers that cell, when the read fails, or when the USB host
// owns the card and the tile is not already cached.
const uint8_t* ensureTileLoaded(uint64_t cell, size_t& outLen) {
    for (int i = 0; i < CACHE_N; ++i) {
        if (g_cache[i].buf && g_cache[i].cell == cell) {
            g_cache[i].stamp = ++g_clock;
            outLen = g_cache[i].len;
            return g_cache[i].buf;
        }
    }
    if (usb_storage::hostActive()) { outLen = 0; return nullptr; }
    if (!tileExists(cell)) { outLen = 0; return nullptr; }

    char path[96];
    tilePathForCell(cell, path, sizeof(path));
    sdLock();
    File f = SD.open(path, FILE_READ);
    size_t len = f ? f.size() : 0;
    if (!f || len < 36) {
        if (f) f.close();
        sdUnlock();
        forgetPresence(cell);        // it went away, or never was
        outLen = 0;
        return nullptr;
    }

    // Make room BEFORE allocating, so the peak is the budget rather than the
    // budget plus the tile being read. A tile bigger than the whole budget
    // empties the cache and lives there alone rather than failing to load.
    auto freeSlot = [&]() -> CachedTile* {
        for (int i = 0; i < CACHE_N; ++i)
            if (!g_cache[i].buf) return &g_cache[i];
        return nullptr;
    };
    auto evictLru = [&]() -> bool {
        CachedTile* lru = nullptr;
        for (int i = 0; i < CACHE_N; ++i)
            if (g_cache[i].buf && (!lru || g_cache[i].stamp < lru->stamp))
                lru = &g_cache[i];
        if (!lru) return false;
        heap_caps_free(lru->buf);
        g_cacheBytes -= lru->len;
        *lru = {};
        return true;
    };
    while ((!freeSlot() || g_cacheBytes + len > CACHE_BYTES) && evictLru()) {}
    CachedTile* victim = freeSlot();

    uint8_t* buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (buf && f.read(buf, len) != len) { heap_caps_free(buf); buf = nullptr; }
    f.close();
    sdUnlock();
    if (!buf) {
        diag::log("map: tile %s (%u KB) would not fit in PSRAM (%u free)", path,
                  (unsigned)(len / 1024),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        outLen = 0;
        return nullptr;
    }
    // Never null: the loop above stops only once a slot is free, or once the
    // cache is empty — and an empty cache is CACHE_N free slots.
    victim->buf = buf; victim->len = len; victim->cell = cell;
    victim->stamp = ++g_clock;
    g_cacheBytes += len;
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

void begin(double lat, double lon, ScanProgress onProgress) {
    MapGuard g;
    (void)onProgress;    // nothing long enough left to report progress on
    sdLock();
    if (!SD.exists(MAP_DIR)) SD.mkdir(MAP_DIR);
    sdUnlock();
    // No tile scan. Tiles are found by computing their H3 ids from the
    // position, so boot only has to index the handful of whole maps.
    scanMaps();
    if (!loadCovering(lat, lon)) clearPrimary();   // no fallback map
}

// Hand the tile cache's PSRAM back. Costs the next few frames an SD re-read;
// buys a large contiguous allocation elsewhere a chance of succeeding.
void releaseCache() {
    MapGuard g;
    for (int i = 0; i < CACHE_N; ++i) {
        if (!g_cache[i].buf) continue;
        heap_caps_free(g_cache[i].buf);
        g_cache[i] = {};
    }
    g_cacheBytes = 0;
}

void rescanCard() {
    MapGuard g;
    // A host computer may have added or removed tiles behind our back, so the
    // "is there a tile for this cell?" answers have to go.
    forgetAllPresence();
    for (int i = 0; i < CACHE_N; ++i) {
        if (!g_cache[i].buf) continue;
        heap_caps_free(g_cache[i].buf);
        g_cache[i] = {};
    }
    g_cacheBytes = 0;
    scanMaps();
}

// Called once per rendered frame (~1 Hz), including the whole time the rider is
// outside the downloaded area — so it must not touch the SD card unless there
// is actually a map to load. The recorder appends to the FIT on the same SPI
// bus under the same mutex once a second, and a ride must never be held up by
// map bookkeeping. Hence the RAM-resident bounds index.
void ensureForPosition(double lat, double lon) {
    MapGuard g;
    if (boundsCover(lat, lon)) return;      // current whole map already covers us
    if (loadCovering(lat, lon)) return;     // switched to a better downloaded map
    clearPrimary();                         // nothing whole-map covers us now
}

void renderInto(double lat, double lon, float metersPerPixel, int centerX,
                int centerY, float rotateDeg, MapScreenData& out) {
    // Held for the whole frame: the projector reads straight out of the tile
    // cache, so nothing may rebuild the index or free a blob underneath it.
    MapGuard g;
    map_tiles::beginProject(out);

    // Which tiles does this frame need? Computed from the viewport, not looked
    // up: the H3 cells covering it ARE the filenames. No card access, and
    // nothing had to be indexed for this to work (see map_select.h, which
    // tools/map_test drives directly).
    // Static, not stack: the UI task has 8 KB and this runs under the whole
    // renderer. renderInto is UI-task-only, like elevationAt.
    static uint64_t sel[MAP_TILE_BUDGET];
    int wanted = 0;
    int lim = mapSelectCells(lat, lon, metersPerPixel, rotateDeg,
                             MAP_TILE_BUDGET, sel, &wanted);

    int drawn = 0;
    for (int a = 0; a < lim; ++a) {
        size_t len;
        const uint8_t* b = ensureTileLoaded(sel[a], len);
        if (!b) continue;              // nothing downloaded for that cell
        drawn++;
        map_tiles::projectBlobInto(b, len, lat, lon, metersPerPixel,
                                   centerX, centerY, rotateDeg);
    }

    int tilePolys = map_tiles::projectedPolyCount();   // split for diagnostics

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
    out.projectedTiles = drawn;
    out.wantedTiles = wanted;
    out.tilePolys = tilePolys;
    map_tiles::projectedClassCounts(out.clsCount);
    if (wanted > lim) {
        // Say so rather than just drawing less: a frame missing its outer tiles
        // looks like ground nobody downloaded.
        static uint32_t lastWarn = 0;
        if (millis() - lastWarn > 10000) {
            lastWarn = millis();
            diag::log("map: viewport needs %d tiles, budget is %d — outer %d "
                      "not drawn", wanted, MAP_TILE_BUDGET, wanted - lim);
        }
    }
}

bool saveAndActivate(const char* name, const uint8_t* data, size_t len) {
    MapGuard g;
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
    MapGuard g;
    if (len < 36 || memcmp(data, "EBM2", 4) != 0) {
        diag::log("tile save rejected: not EBM2 (%u bytes)", (unsigned)len);
        return false;
    }
    // Strip the ".ebm" the caller may have included, then split the id: the
    // directory carries the first TILE_PREFIX_LEN characters and the filename
    // carries the rest, so the prefix is not repeated in every name.
    char bare[48];
    snprintf(bare, sizeof(bare), "%.40s", id);
    if (char* dot = strstr(bare, ".ebm")) *dot = 0;

    char dir[80];
    tileDirFor(bare, dir, sizeof(dir));
    const char* leaf = (strlen(dir) > strlen(TILE_DIR)) ? bare + TILE_PREFIX_LEN
                                                        : bare;
    char path[96];
    snprintf(path, sizeof(path), "%s/%s.ebm", dir, leaf);

    sdLock();
    if (!SD.exists(MAP_DIR)) SD.mkdir(MAP_DIR);
    if (!SD.exists(TILE_DIR)) SD.mkdir(TILE_DIR);
    // Remember the last directory we ensured exists. A bulk transfer is one
    // geographic area, so this collapses to a SINGLE exists+mkdir for the whole
    // batch instead of a pair of FAT metadata operations per tile — fewer writes
    // to be interrupted, which is how the previous layout helped corrupt a card.
    {
        static char ensured[80] = "";
        if (strcmp(ensured, dir) != 0) {
            if (!SD.exists(dir)) SD.mkdir(dir);
            snprintf(ensured, sizeof(ensured), "%s", dir);
        }
    }
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
    // Renders immediately: forget the cached "no tile here" and any stale blob
    // for this cell, and the next frame that wants it reads the new file.
    if (uint64_t cell = h3_from_id(bare)) {
        forgetPresence(cell);
        invalidateCached(cell);
    }
    return true;
}

bool hasTile(const char* id) {
    MapGuard g;
    char bare[32];
    snprintf(bare, sizeof(bare), "%.24s", id);
    if (char* dot = strstr(bare, ".ebm")) *dot = 0;
    uint64_t cell = h3_from_id(bare);
    return cell && tileExists(cell);
}

bool coversPosition(double lat, double lon) {
    MapGuard g;
    uint64_t cell = h3_cell_at(lat, lon);
    if (cell && tileExists(cell)) return true;
    return haveBounds && lat >= loadedS && lat <= loadedN &&
           lon >= loadedW && lon <= loadedE;
}

float elevationAt(double lat, double lon) {
    MapGuard g;                 // shares the tile cache with renderInto
    uint64_t cell = h3_cell_at(lat, lon);
    if (!cell) return NAN;
    size_t len;
    const uint8_t* b = ensureTileLoaded(cell, len);
    if (!b) return NAN;
    return elevFromBlob(b, len, lat, lon);
}

// The one call that still walks the card, because "what have you got?" cannot
// be computed — the phone asks so it can skip re-sending tiles the device
// already holds. Cheap by comparison with the old boot scan: this reads
// directory NAMES only and never opens a tile, since the name is the id.
// On demand, from the BLE server task, never on the boot path.
int listTileIds(char out[][24], int maxOut) {
    MapGuard g;
    int n = 0;
    sdLock();
    auto take = [&](const char* prefix, const char* base) {
        if (n >= maxOut || !strstr(base, ".ebm")) return;
        snprintf(out[n], 24, "%s%s", prefix, base);
        if (char* dot = strstr(out[n], ".ebm")) *dot = 0;
        n++;
    };
    auto walk = [&](const char* path, const char* prefix) {
        File d = SD.open(path);
        if (!d) return;
        for (File f = d.openNextFile(); f && n < maxOut; f = d.openNextFile()) {
            if (!f.isDirectory()) {
                const char* nm = f.name();
                const char* base = strrchr(nm, '/');
                take(prefix, base ? base + 1 : nm);
            }
            f.close();
        }
        d.close();
    };
    walk(TILE_DIR, "");                       // legacy flat cards
    File d = SD.open(TILE_DIR);
    if (d) {
        for (File e = d.openNextFile(); e && n < maxOut; e = d.openNextFile()) {
            if (e.isDirectory()) {
                const char* nm = e.name();
                const char* base = strrchr(nm, '/');
                char sub[16], full[96];
                snprintf(sub, sizeof(sub), "%s", base ? base + 1 : nm);
                snprintf(full, sizeof(full), "%s/%s", TILE_DIR, sub);
                e.close();
                walk(full, strlen(sub) == TILE_PREFIX_LEN ? sub : "");
                continue;
            }
            e.close();
        }
        d.close();
    }
    sdUnlock();
    return n;
}

int tileCount() {
    static char ids[1][24];
    (void)ids;
    // Counting means walking the card, so this is for diagnostics only — never
    // call it anywhere a boot is waiting.
    MapGuard g;
    int n = 0;
    sdLock();
    File d = SD.open(TILE_DIR);
    if (d) {
        for (File e = d.openNextFile(); e; e = d.openNextFile()) {
            if (e.isDirectory()) {
                const char* nm = e.name();
                const char* base = strrchr(nm, '/');
                char full[96];
                snprintf(full, sizeof(full), "%s/%s", TILE_DIR, base ? base + 1 : nm);
                e.close();
                File s2 = SD.open(full);
                if (s2) {
                    for (File f = s2.openNextFile(); f; f = s2.openNextFile()) {
                        if (!f.isDirectory() && strstr(f.name(), ".ebm")) n++;
                        f.close();
                    }
                    s2.close();
                }
                continue;
            }
            if (strstr(e.name(), ".ebm")) n++;
            e.close();
        }
        d.close();
    }
    sdUnlock();
    return n;
}

uint32_t sdFreeKB() {
    sdLock();
    uint32_t kb = (uint32_t)((SD.totalBytes() - SD.usedBytes()) / 1024);
    sdUnlock();
    return kb;
}

int listMaps(MapBounds* out, int maxOut) {
    MapGuard g;
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
