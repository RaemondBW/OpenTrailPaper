#include "routes.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>

#include "config.h"
#include "sd_bus.h"

#define ROUTE_DIR "/routes"

namespace {

constexpr int MAX_PTS = 4096;

float* latArr = nullptr;
float* lonArr = nullptr;
float* cumM = nullptr;      // cumulative distance from route start (m)
int nPts = 0;
char curName[routes::MAX_NAME] = "";
int progIdx = 0;
// Continuous along-route distance (m). progIdx is still the nearest vertex (the
// map view and the route overlay want an index), but every distance decision —
// turn countdown, cursor advance, remaining — uses this, so it tracks the rider
// instead of quantising to the point spacing.
float progAlongM = 0.0f;

// Turn-by-turn maneuvers
constexpr int MAX_MANEUVERS = 128;
struct Maneuver {
    double lat, lon;
    char instr[routes::MANEUVER_TEXT];
};
Maneuver maneuvers[MAX_MANEUVERS];
// Along-route distance (m) of each maneuver, from projecting it onto the route
// polyline. Continuous, NOT a vertex index: a real router puts maneuvers at
// road intersections, and a route polyline only carries a vertex where the road
// BENDS — so "continue onto X" (a name change with no bend) has no vertex
// anywhere near it, and on a sparse polyline the nearest vertex can be hundreds
// of metres away. Snapping to it moved the turn that far up or down the route.
float maneuverAlongM[MAX_MANEUVERS];
int nManeuvers = 0;
int curManeuver = 0;       // next maneuver ahead of the rider
bool pendingNav = false;
bool activeNav = false;
bool navDismissed = false;        // user tapped the banner away; don't re-grab
float offRouteM = 99999.0f;       // rider's distance to the nearest route point
int offRouteFixes = 0;            // consecutive fixes far from the window match

double haversineM(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371000.0;
    double dLat = radians(lat2 - lat1);
    double dLon = radians(lon2 - lon1);
    double a = sin(dLat / 2) * sin(dLat / 2) +
               cos(radians(lat1)) * cos(radians(lat2)) *
               sin(dLon / 2) * sin(dLon / 2);
    return R * 2 * atan2(sqrt(a), sqrt(1 - a));
}

// Nearest route point to (lat,lon) within [from,to); returns its index and the
// squared distance (m^2) via outDist2. Fast planar approximation.
int nearestRouteIdx(double lat, double lon, int from, int to, float& outDist2) {
    float best = 1e18f;
    int bestI = -1;
    float cosL = (float)cos(lat * M_PI / 180.0);
    for (int i = from; i < to; ++i) {
        float dy = (float)((lat - latArr[i]) * 110540.0);
        float dx = (float)((lon - lonArr[i]) * 111320.0 * cosL);
        float d2 = dx * dx + dy * dy;
        if (d2 < best) { best = d2; bestI = i; }
    }
    outDist2 = best;
    return bestI;
}

// Project (lat,lon) onto the polyline SEGMENT from point i to i+1. Returns the
// squared distance (m^2) and, via outT, how far along that segment the foot of
// the perpendicular falls (clamped to 0..1).
//
// Snapping to the nearest vertex instead — which is what this used to do —
// quantises the rider's along-route position to the point spacing. After
// decimation that spacing can be tens of metres, so the distance-to-turn
// readout sticks and then jumps rather than counting down, and a maneuver can
// sit at "0 m" while the rider is already well past it. That is what made
// turn-by-turn look unrelated to actual position.
float projectOnSegment(double lat, double lon, int i, float cosL, float& outT) {
    float ay = (float)((latArr[i] - lat) * 110540.0);
    float ax = (float)((lonArr[i] - lon) * 111320.0 * cosL);
    float by = (float)((latArr[i + 1] - lat) * 110540.0);
    float bx = (float)((lonArr[i + 1] - lon) * 111320.0 * cosL);
    float vx = bx - ax, vy = by - ay;
    float len2 = vx * vx + vy * vy;
    float t = 0.0f;
    if (len2 > 1e-6f) {
        t = -(ax * vx + ay * vy) / len2;       // foot of the perpendicular
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
    }
    float px = ax + vx * t, py = ay + vy * t;  // closest point, rider-relative
    outT = t;
    return px * px + py * py;
}

// Search window around the rider's last known along-route position, in metres.
// Forward is generous because it is not a speed limit — you can join a route
// mid-way, or come out of a tunnel — while backward only has to be able to
// undo a bad fix.
constexpr float WINDOW_BACK_M = 300.0f;
constexpr float WINDOW_FWD_M = 2000.0f;

// First route index at or past `m` metres along. cumM is monotonic, so binary
// search. Clamped to a valid index.
int indexAtAlong(float m) {
    if (nPts <= 1) return 0;
    if (m <= 0) return 0;
    if (m >= cumM[nPts - 1]) return nPts - 1;
    int lo = 0, hi = nPts - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (cumM[mid] < m) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// Nearest segment to (lat,lon) over points [from,to). Returns the segment's
// start index (or -1), with the along-segment fraction and squared distance.
int nearestSegment(double lat, double lon, int from, int to, float& outT,
                   float& outDist2) {
    float best = 1e18f, bestT = 0.0f;
    int bestI = -1;
    float cosL = (float)cos(lat * M_PI / 180.0);
    for (int i = from; i + 1 < to; ++i) {
        float t;
        float d2 = projectOnSegment(lat, lon, i, cosL, t);
        if (d2 < best) { best = d2; bestT = t; bestI = i; }
    }
    outT = bestT;
    outDist2 = best;
    return bestI;
}

// Place each maneuver on the loaded route as an along-route distance, so turns
// can be tracked by distance travelled (robust) instead of GPS proximity.
//
// PROJECTED onto the route's segments, not snapped to the nearest vertex. A
// router puts maneuvers at road intersections and a route polyline carries a
// vertex only where the road bends, so a "continue onto X" (name change, no
// bend) has no vertex near it at all — snapping put it at whatever vertex
// happened to be closest, which on the sparse polylines MapKit returns can be
// hundreds of metres up or down the route. That alone announced the wrong turn.
//
// Maneuvers arrive in route order, so we place them monotonically: each search
// starts at the segment holding the previous maneuver and runs forward. Without
// this, a maneuver on a returning/overlapping leg (out-and-backs, lollipop
// loops, routes that cross themselves) could land on an *earlier* leg that
// passes nearby, giving it too small a distance and making
// advanceManeuverCursor skip several turns at once.
void mapManeuversToRoute() {
    int fromIdx = 0;
    float prevAlong = -1.0f;
    for (int i = 0; i < nManeuvers; ++i) {
        float t = 0, d2;
        int seg = nPts >= 2 ? nearestSegment(maneuvers[i].lat, maneuvers[i].lon,
                                             fromIdx, nPts, t, d2)
                            : -1;
        float along = seg >= 0
                          ? cumM[seg] + t * (cumM[seg + 1] - cumM[seg])
                          : (prevAlong > 0 ? prevAlong : 0.0f);
        // STRICTLY increasing. Two maneuvers that project to the same spot — a
        // slip road, a quick left-then-right — would otherwise share a distance
        // and be consumed together by advanceManeuverCursor(), silently
        // dropping the second instruction. Covered by testCloselySpacedTurns.
        if (i > 0 && along <= prevAlong) along = prevAlong + 1.0f;
        if (nPts >= 2 && along > cumM[nPts - 1]) along = cumM[nPts - 1];
        maneuverAlongM[i] = along;
        prevAlong = along;
        if (seg >= 0) fromIdx = seg;   // the next maneuver is at or after this one
    }
}

// How far past a maneuver the rider must be before it stops being "the next
// turn". Purely a display rule — nothing is remembered — but it has to clear
// GPS noise: at 5 m (what this used to be) a single noisy fix near a corner
// projects onto the leg AFTER the turn and the cue jumps ahead while the rider
// is still short of the junction. 20 m is past the noise and still well inside
// a city block, so a completed turn simply stays up for a few seconds after you
// take it, which is what a rider wants anyway.
//
// Capped at 45% of the gap to the next maneuver so a staggered junction (two
// turns 20-30 m apart) still hands over at the midpoint instead of holding the
// first one until the rider is past the second.
constexpr float kManeuverPastM = 20.0f;

float maneuverPastMargin(int i) {
    float margin = kManeuverPastM;
    if (i + 1 < nManeuvers) {
        float half = (maneuverAlongM[i + 1] - maneuverAlongM[i]) * 0.45f;
        if (half < margin) margin = half;
    }
    return margin < 2.0f ? 2.0f : margin;
}

// The next turn is DERIVED from where the rider is, every fix. It is not
// tracked, advanced or remembered.
//
// It used to be a cursor that only ever moved forward, and that is what made
// the turn cues untrustworthy: one bad fix consumed the turn the rider was
// still riding towards, and no later fix could undo it. Standing still with a
// wandering fix, the cues walked through the route one maneuver at a time,
// which looks exactly like a countdown driven by time rather than by distance.
//
// Being a pure function of position means the readout is always the real
// distance from where the rider is now to the turn ahead of them. A bad fix is
// wrong for exactly one fix and then corrects itself.
int maneuverAheadOf(float alongM) {
    int i = 0;
    while (i < nManeuvers && alongM > maneuverAlongM[i] + maneuverPastMargin(i)) i++;
    return i;
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
    sdLock();
    if (!SD.exists(ROUTE_DIR)) SD.mkdir(ROUTE_DIR);
    sdUnlock();
    return true;
}

int list(char names[][MAX_NAME], int maxNames) {
    sdLock();
    File dir = SD.open(ROUTE_DIR);
    if (!dir) { sdUnlock(); return 0; }
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
    sdUnlock();
    return n;
}

bool load(const char* filename) {
    if (!ensureBuffers()) return false;

    char path[64];
    snprintf(path, sizeof(path), ROUTE_DIR "/%s", filename);
    sdLock();
    File f = SD.open(path, FILE_READ);
    if (!f) {
        sdUnlock();
        Serial.printf("[route] can't open %s\n", path);
        return false;
    }

    nPts = 0;
    progIdx = 0;
    progAlongM = 0.0f;
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
    sdUnlock();

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
    progAlongM = 0.0f;
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
    sdLock();
    if (SD.exists(ROUTE_DIR) || SD.mkdir(ROUTE_DIR)) {
        char path[64];
        snprintf(path, sizeof(path), ROUTE_DIR "/%s", name);
        File f = SD.open(path, FILE_WRITE);
        if (f) {
            f.write((const uint8_t*)gpx, len);
            f.close();
        }
    }
    sdUnlock();

    Serial.printf("[route] uploaded %s: %d pts, %.1f km\n", name, nPts,
                  cumM[nPts - 1] / 1000.0f);
    return true;
}

void clearRoute() {
    nPts = 0;
    progIdx = 0;
    progAlongM = 0.0f;
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
    int prev = progIdx;

    // Project the rider onto the route, searching a window around where they
    // were. The window exists only to disambiguate a route that passes near
    // ITSELF (out-and-backs, lollipop loops, self-crossings) — position alone
    // genuinely cannot say which leg you are on there.
    //
    // Measured in METRES, and symmetric enough to be undone. It used to be
    // "8 points back, 120 points forward", but a route's points are not evenly
    // spaced — a router's polyline is dense through curves and can run a
    // kilometre of straight on two points — so that was a 16 m window on one
    // stretch and a 20 km one on the next. Where the backward allowance was
    // small, the match could move forward and never come back, and the position
    // ratcheted down the route while the rider stood still.
    int from = indexAtAlong(progAlongM - WINDOW_BACK_M);
    int to   = indexAtAlong(progAlongM + WINDOW_FWD_M) + 2;   // exclusive
    if (from > 0) from--;              // include the segment spanning the bound
    if (to > nPts) to = nPts;
    float d2, segT;
    int seg = nearestSegment(lat, lon, from, to, segT, d2);
    // Single-point route (or a window with no segment): fall back to the vertex.
    int idx = seg >= 0 ? seg : nearestRouteIdx(lat, lon, from, to, d2);

    // Whole-route re-acquire, for joining mid-route or rejoining after a real
    // detour. This is the dangerous case: on a route that comes back near
    // itself, the globally-nearest point can be a *later* leg sitting next to
    // the rider, which would yank progIdx forward and run the turn prompts
    // several maneuvers ahead. Guard it so it can't fire on GPS jitter:
    //   - only after we've been clearly off the window for a few fixes, and
    //   - only if the global match is decisively closer than the window match
    //     (fd2 < d2/4 == less than half the distance).
    if (d2 > 60.0f * 60.0f) {
        if (++offRouteFixes >= 4) {
            float fd2, ft;
            int fseg = nearestSegment(lat, lon, 0, nPts, ft, fd2);
            if (fseg >= 0 && fd2 < d2 * 0.25f) {
                idx = fseg;
                segT = ft;
                d2 = fd2;
                offRouteFixes = 0;
            }
        }
    } else {
        offRouteFixes = 0;
    }
    if (idx >= 0) {
        progIdx = idx;
        // Interpolate along the matched segment so the along-route position is
        // continuous rather than quantised to the vertex spacing.
        progAlongM = (idx + 1 < nPts)
                         ? cumM[idx] + segT * (cumM[idx + 1] - cumM[idx])
                         : cumM[idx];
    }
    offRouteM = sqrtf(d2);

    // Grab the route into the turn view when we're clearly on it and moving
    // along it (projection advanced forward), unless the user dismissed it.
    bool movingAlong = progIdx > prev;
    if (nManeuvers > 0 && !activeNav && !navDismissed &&
        offRouteM < 40.0f && movingAlong) {
        activeNav = true;
        pendingNav = false;
        curManeuver = 0;
    }

    // Recomputed from the rider's position, not advanced from its old value.
    if (activeNav) curManeuver = maneuverAheadOf(progAlongM);
}

// --- Maneuvers / navigation ---------------------------------------------

void clearManeuvers() {
    nManeuvers = 0;
    curManeuver = 0;
    pendingNav = false;
    activeNav = false;
    navDismissed = false;
}

void addManeuver(double lat, double lon, const char* instruction) {
    if (nManeuvers >= MAX_MANEUVERS) return;
    Maneuver& m = maneuvers[nManeuvers++];
    m.lat = lat;
    m.lon = lon;
    snprintf(m.instr, sizeof(m.instr), "%s", instruction ? instruction : "");
}

void finishManeuvers() {
    mapManeuversToRoute();          // route geometry is already loaded by now
    pendingNav = nManeuvers > 0;
    activeNav = false;
    navDismissed = false;
    curManeuver = 0;
}

int maneuverCount() { return nManeuvers; }

bool navPending() { return pendingNav; }

void startNav() {
    if (nManeuvers > 0) {
        mapManeuversToRoute();
        activeNav = true;
        navDismissed = false;
        // From the rider's current position, so turns already behind them are
        // not announced when navigation starts mid-route.
        curManeuver = maneuverAheadOf(progAlongM);
    }
    pendingNav = false;
}

void dismissNav() {
    pendingNav = false;
    activeNav = false;
    navDismissed = true;            // don't auto-grab again until a new route
}

bool navActive() { return activeNav; }

bool nextTurn(char* instruction, int textLen, float& distanceM) {
    if (!activeNav || curManeuver >= nManeuvers || nPts == 0) return false;
    // Along-route distance from the rider's projected position to the turn.
    float along = maneuverAlongM[curManeuver] - progAlongM;
    if (along < 0) along = 0;
    distanceM = along;
    snprintf(instruction, textLen, "%s", maneuvers[curManeuver].instr);
    return true;
}

int upcomingCount() {
    if (!activeNav || nPts == 0) return 0;
    return nManeuvers - curManeuver;
}

bool upcomingTurn(int i, char* instruction, int textLen, float& distanceM) {
    int m = curManeuver + i;
    if (!activeNav || i < 0 || m >= nManeuvers || nPts == 0) return false;
    float along = maneuverAlongM[m] - progAlongM;
    if (along < 0) along = 0;
    distanceM = along;
    snprintf(instruction, textLen, "%s", maneuvers[m].instr);
    return true;
}

int progressIndex() { return progIdx; }

float remainingKm() {
    if (!active()) return 0;
    return (cumM[nPts - 1] - progAlongM) / 1000.0f;
}

}  // namespace routes
