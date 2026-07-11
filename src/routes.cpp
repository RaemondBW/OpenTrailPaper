#include "routes.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>

#include "config.h"

#define ROUTE_DIR "/routes"

namespace {

constexpr int MAX_PTS = 4096;

float* latArr = nullptr;
float* lonArr = nullptr;
float* cumM = nullptr;      // cumulative distance from route start (m)
int nPts = 0;
char curName[routes::MAX_NAME] = "";
int progIdx = 0;

// Turn-by-turn maneuvers
constexpr int MAX_MANEUVERS = 128;
struct Maneuver {
    double lat, lon;
    char instr[routes::MANEUVER_TEXT];
};
Maneuver maneuvers[MAX_MANEUVERS];
int nManeuvers = 0;
int curManeuver = 0;       // next maneuver ahead of the rider
bool pendingNav = false;
bool activeNav = false;

double haversineM(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371000.0;
    double dLat = radians(lat2 - lat1);
    double dLon = radians(lon2 - lon1);
    double a = sin(dLat / 2) * sin(dLat / 2) +
               cos(radians(lat1)) * cos(radians(lat2)) *
               sin(dLon / 2) * sin(dLon / 2);
    return R * 2 * atan2(sqrt(a), sqrt(1 - a));
}

bool ensureBuffers() {
    if (latArr) return true;
    latArr = (float*)heap_caps_malloc(MAX_PTS * sizeof(float), MALLOC_CAP_SPIRAM);
    lonArr = (float*)heap_caps_malloc(MAX_PTS * sizeof(float), MALLOC_CAP_SPIRAM);
    cumM = (float*)heap_caps_malloc(MAX_PTS * sizeof(float), MALLOC_CAP_SPIRAM);
    return latArr && lonArr && cumM;
}

// Streaming attribute scan: find `lat="` / `lon="` pairs after trkpt/rtept
// tags. GPX in practice always orders lat before lon.
struct GpxScanner {
    int stride = 1;      // decimation: keep every stride-th point
    int seen = 0;

    void addPoint(double lat, double lon) {
        if (seen++ % stride) return;
        if (nPts >= MAX_PTS) {
            // halve in place, double the stride, keep going
            for (int i = 0; i < MAX_PTS / 2; ++i) {
                latArr[i] = latArr[i * 2];
                lonArr[i] = lonArr[i * 2];
            }
            nPts = MAX_PTS / 2;
            stride *= 2;
        }
        latArr[nPts] = (float)lat;
        lonArr[nPts] = (float)lon;
        nPts++;
    }
};

}  // namespace

namespace routes {

bool begin() {
    if (!SD.exists(ROUTE_DIR)) SD.mkdir(ROUTE_DIR);
    return true;
}

int list(char names[][MAX_NAME], int maxNames) {
    File dir = SD.open(ROUTE_DIR);
    if (!dir) return 0;
    int n = 0;
    for (File f = dir.openNextFile(); f && n < maxNames;
         f = dir.openNextFile()) {
        if (!f.isDirectory()) {
            const char* base = strrchr(f.name(), '/');
            base = base ? base + 1 : f.name();
            size_t len = strlen(base);
            if (len > 4 && strcasecmp(base + len - 4, ".gpx") == 0) {
                snprintf(names[n], MAX_NAME, "%s", base);
                n++;
            }
        }
        f.close();
    }
    dir.close();
    return n;
}

bool load(const char* filename) {
    if (!ensureBuffers()) return false;

    char path[64];
    snprintf(path, sizeof(path), ROUTE_DIR "/%s", filename);
    File f = SD.open(path, FILE_READ);
    if (!f) {
        Serial.printf("[route] can't open %s\n", path);
        return false;
    }

    nPts = 0;
    progIdx = 0;
    GpxScanner sc;

    // Chunked scan with carryover so attributes split across chunk
    // boundaries aren't lost.
    static char buf[2048 + 64];
    int carry = 0;
    bool havePt = false;
    double lat = 0;

    while (true) {
        int rd = f.read((uint8_t*)buf + carry, 2048);
        if (rd <= 0) break;
        int len = carry + rd;
        buf[len] = 0;

        char* p = buf;
        while ((p = strstr(p, havePt ? "lon=\"" : "lat=\"")) != nullptr) {
            // don't parse an attribute that may be cut off at the end
            if (p + 24 > buf + len) break;
            double v = strtod(p + 5, nullptr);
            if (!havePt) {
                lat = v;
                havePt = true;
            } else {
                sc.addPoint(lat, v);
                havePt = false;
            }
            p += 5;
        }

        // keep the tail in case an attribute straddles the boundary
        carry = len > 64 ? 64 : len;
        memmove(buf, buf + len - carry, carry);
    }
    f.close();

    if (nPts < 2) {
        Serial.printf("[route] %s: no points found\n", filename);
        nPts = 0;
        return false;
    }

    cumM[0] = 0;
    for (int i = 1; i < nPts; ++i) {
        cumM[i] = cumM[i - 1] + (float)haversineM(latArr[i - 1], lonArr[i - 1],
                                                  latArr[i], lonArr[i]);
    }
    snprintf(curName, sizeof(curName), "%s", filename);
    Serial.printf("[route] %s: %d pts, %.1f km\n", filename, nPts,
                  cumM[nPts - 1] / 1000.0f);
    return true;
}

bool loadFromMemory(const char* name, const char* gpx, size_t len) {
    if (!ensureBuffers() || !gpx || len < 16) return false;

    nPts = 0;
    progIdx = 0;
    GpxScanner sc;

    // gpx is NUL-terminated by the caller, so strstr is safe. Scan for the
    // lat/lon attribute pairs (GPX always orders lat before lon).
    bool havePt = false;
    double lat = 0;
    const char* p = gpx;
    while ((p = strstr(p, havePt ? "lon=\"" : "lat=\"")) != nullptr) {
        double v = strtod(p + 5, nullptr);
        if (!havePt) {
            lat = v;
            havePt = true;
        } else {
            sc.addPoint(lat, v);
            havePt = false;
        }
        p += 5;
    }

    if (nPts < 2) {
        Serial.println("[route] uploaded GPX had no points");
        nPts = 0;
        return false;
    }

    cumM[0] = 0;
    for (int i = 1; i < nPts; ++i) {
        cumM[i] = cumM[i - 1] + (float)haversineM(latArr[i - 1], lonArr[i - 1],
                                                  latArr[i], lonArr[i]);
    }
    snprintf(curName, sizeof(curName), "%s", name);

    // Persist to SD when a card is present so it survives a reboot.
    if (SD.exists(ROUTE_DIR) || SD.mkdir(ROUTE_DIR)) {
        char path[64];
        snprintf(path, sizeof(path), ROUTE_DIR "/%s", name);
        File f = SD.open(path, FILE_WRITE);
        if (f) {
            f.write((const uint8_t*)gpx, len);
            f.close();
        }
    }

    Serial.printf("[route] uploaded %s: %d pts, %.1f km\n", name, nPts,
                  cumM[nPts - 1] / 1000.0f);
    return true;
}

void clearRoute() {
    nPts = 0;
    progIdx = 0;
    curName[0] = 0;
    // Clearing a route also ends any navigation on it, otherwise the turn
    // banner would linger with stale maneuvers.
    nManeuvers = 0;
    curManeuver = 0;
    pendingNav = false;
    activeNav = false;
}

bool active() { return nPts >= 2; }
const char* activeName() { return curName; }
int pointCount() { return nPts; }

void point(int i, double& lat, double& lon) {
    lat = latArr[i];
    lon = lonArr[i];
}

void updateProgress(double lat, double lon) {
    if (!active()) return;
    // search a window ahead of the current index; advance to the nearest
    // route point within 80 m
    int end = progIdx + 60;
    if (end > nPts) end = nPts;
    float best = 80.0f * 80.0f;
    int bestI = -1;
    for (int i = progIdx; i < end; ++i) {
        // fast approximate distance (meters) — fine at this scale
        float dy = (float)((lat - latArr[i]) * 110540.0);
        float dx = (float)((lon - lonArr[i]) * 111320.0 *
                           cos(lat * M_PI / 180.0));
        float d2 = dx * dx + dy * dy;
        if (d2 < best) {
            best = d2;
            bestI = i;
        }
    }
    if (bestI > progIdx) progIdx = bestI;

    // Advance the maneuver cursor: once we pass within 20 m of the next
    // turn, the following one becomes current.
    if (activeNav && curManeuver < nManeuvers) {
        double d = haversineM(lat, lon, maneuvers[curManeuver].lat,
                              maneuvers[curManeuver].lon);
        if (d < 20.0) curManeuver++;
    }
}

// --- Maneuvers / navigation ---------------------------------------------

void clearManeuvers() {
    nManeuvers = 0;
    curManeuver = 0;
    pendingNav = false;
    activeNav = false;
}

void addManeuver(double lat, double lon, const char* instruction) {
    if (nManeuvers >= MAX_MANEUVERS) return;
    Maneuver& m = maneuvers[nManeuvers++];
    m.lat = lat;
    m.lon = lon;
    snprintf(m.instr, sizeof(m.instr), "%s", instruction ? instruction : "");
}

void finishManeuvers() {
    pendingNav = nManeuvers > 0;
    activeNav = false;
    curManeuver = 0;
}

int maneuverCount() { return nManeuvers; }

bool navPending() { return pendingNav; }

void startNav() {
    if (nManeuvers > 0) {
        activeNav = true;
        curManeuver = 0;
    }
    pendingNav = false;
}

void dismissNav() {
    pendingNav = false;
    activeNav = false;
}

bool navActive() { return activeNav; }

bool nextTurn(char* instruction, int textLen, float& distanceM) {
    if (!activeNav || curManeuver >= nManeuvers) return false;
    // Distance from the rider's last known route point to the next turn.
    double rlat = latArr[progIdx], rlon = lonArr[progIdx];
    distanceM = (float)haversineM(rlat, rlon, maneuvers[curManeuver].lat,
                                  maneuvers[curManeuver].lon);
    snprintf(instruction, textLen, "%s", maneuvers[curManeuver].instr);
    return true;
}

int progressIndex() { return progIdx; }

float remainingKm() {
    if (!active()) return 0;
    return (cumM[nPts - 1] - cumM[progIdx]) / 1000.0f;
}

}  // namespace routes
