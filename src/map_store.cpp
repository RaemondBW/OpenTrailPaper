#include "map_store.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <cstring>

#include "map_tiles.h"
#include "diag.h"

// Embedded default map (board_build.embed_files = data/sf.ebm).
extern const uint8_t map_ebm_start[] asm("_binary_data_sf_ebm_start");
extern const uint8_t map_ebm_end[] asm("_binary_data_sf_ebm_end");

namespace {

constexpr char MAP_DIR[] = "/maps";

uint8_t* activeBuf = nullptr;     // PSRAM copy of the active SD map (or null)
size_t activeLen = 0;
bool usingEmbedded = true;

// Bounds of the currently loaded map, so we can tell when to switch.
double loadedS = 0, loadedW = 0, loadedN = 0, loadedE = 0;
bool haveBounds = false;

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
        haveBounds = headerBounds(map_ebm_start, loadedS, loadedW, loadedN, loadedE);
        diag::log("map: embedded default (%u KB)", (unsigned)(len / 1024));
    }
}

// Read a whole .ebm file into a fresh PSRAM buffer and make it the active map.
bool loadFile(const char* path) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    size_t len = f.size();
    if (len < 36) { f.close(); return false; }
    uint8_t* buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!buf) { f.close(); return false; }
    size_t got = f.read(buf, len);
    f.close();
    if (got != len || !map_tiles::load(buf, len)) {
        heap_caps_free(buf);
        return false;
    }
    if (activeBuf) heap_caps_free(activeBuf);   // free the previous SD map
    activeBuf = buf;
    activeLen = len;
    usingEmbedded = false;
    haveBounds = headerBounds(buf, loadedS, loadedW, loadedN, loadedE);
    diag::log("map: loaded %s (%u KB)", path, (unsigned)(len / 1024));
    return true;
}

// Find a downloaded map whose bounds contain (lat, lon); load it if found.
bool loadCovering(double lat, double lon) {
    File dir = SD.open(MAP_DIR);
    if (!dir) return false;
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
    return best[0] ? loadFile(best) : false;
}

}  // namespace

namespace map_store {

void begin(double lat, double lon) {
    if (!SD.exists(MAP_DIR)) SD.mkdir(MAP_DIR);
    if (!loadCovering(lat, lon)) loadEmbedded();
}

void ensureForPosition(double lat, double lon) {
    if (boundsCover(lat, lon)) return;      // current map already covers us
    if (loadCovering(lat, lon)) return;     // switched to a better downloaded map
    if (!usingEmbedded && !haveBounds) loadEmbedded();
}

bool saveAndActivate(const char* name, const uint8_t* data, size_t len) {
    if (len < 36 || memcmp(data, "EBM1", 4) != 0) {
        diag::log("map save rejected: not EBM1 (%u bytes)", (unsigned)len);
        return false;
    }
    if (!SD.exists(MAP_DIR)) SD.mkdir(MAP_DIR);
    char path[80];
    snprintf(path, sizeof(path), "%s/%.48s", MAP_DIR, name);
    if (!strstr(path, ".ebm")) strncat(path, ".ebm", sizeof(path) - strlen(path) - 1);

    SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) { diag::log("map save: can't open %s", path); return false; }
    size_t wrote = 0;
    while (wrote < len) {
        size_t chunk = len - wrote < 4096 ? len - wrote : 4096;
        size_t w = f.write(data + wrote, chunk);
        if (w != chunk) { f.close(); SD.remove(path); return false; }
        wrote += w;
    }
    f.close();
    diag::log("map saved: %s (%u KB)", path, (unsigned)(len / 1024));
    return loadFile(path);
}

uint32_t sdFreeKB() {
    return (uint32_t)((SD.totalBytes() - SD.usedBytes()) / 1024);
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
    return n;
}

}  // namespace map_store
