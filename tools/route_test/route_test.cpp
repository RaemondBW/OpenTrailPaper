// Host tests for the along-route progress and turn-by-turn cursor in
// src/routes.cpp.
//
// The reported problem was "turn-by-turn doesn't seem linked to actual position
// along the route", which is exactly the kind of bug that is miserable to chase
// on a bike and trivial to reproduce here: build a route, walk a synthetic rider
// along it, and assert which turn is being announced and how far away it says it
// is.
//
// Geometry is built in degrees around a reference point, using the same flat
// approximation the firmware uses (110540 m/deg lat, 111320*cos(lat) m/deg lon)
// so distances in the assertions are in metres and easy to reason about.

#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>

#include "routes.h"
#include "power_mgmt.h"

// routes.cpp reaches the SD through sdLock()/sdUnlock(), which hold a
// no-light-sleep PM lock on device. Nothing to do on the host.
SemaphoreHandle_t g_sdMutex = nullptr;
namespace power_mgmt {
void busyAcquire() {}
void busyRelease() {}
}  // namespace power_mgmt

namespace {

int g_fail = 0;
const char* g_case = "";

void check(bool ok, const std::string& what) {
    if (!ok) {
        printf("  FAIL  %s: %s\n", g_case, what.c_str());
        ++g_fail;
    }
}

void checkNear(float got, float want, float tol, const std::string& what) {
    if (!(fabsf(got - want) <= tol)) {
        printf("  FAIL  %s: %s (got %.1f, want %.1f +/- %.1f)\n",
               g_case, what.c_str(), got, want, tol);
        ++g_fail;
    }
}

constexpr double LAT0 = 37.7749, LON0 = -122.4194;
const double M_PER_DEG_LAT = 110540.0;
double mPerDegLon() { return 111320.0 * cos(LAT0 * M_PI / 180.0); }

struct Pt { double lat, lon; };

// Offsets in metres (east, north) from the reference point.
Pt at(double eastM, double northM) {
    return {LAT0 + northM / M_PER_DEG_LAT, LON0 + eastM / mPerDegLon()};
}

// Build a GPX from a point list; routes::loadFromMemory parses lat=/lon= pairs.
std::string gpxOf(const std::vector<Pt>& pts) {
    std::string s = "<?xml version=\"1.0\"?><gpx><trk><trkseg>";
    char buf[128];
    for (const Pt& p : pts) {
        snprintf(buf, sizeof(buf), "<trkpt lat=\"%.7f\" lon=\"%.7f\"></trkpt>",
                 p.lat, p.lon);
        s += buf;
    }
    s += "</trkseg></trk></gpx>";
    return s;
}

// Densify a polyline to ~stepM spacing, like a real GPX track.
std::vector<Pt> densify(const std::vector<Pt>& corners, double stepM) {
    std::vector<Pt> out;
    for (size_t i = 0; i + 1 < corners.size(); ++i) {
        double y0 = (corners[i].lat - LAT0) * M_PER_DEG_LAT;
        double x0 = (corners[i].lon - LON0) * mPerDegLon();
        double y1 = (corners[i + 1].lat - LAT0) * M_PER_DEG_LAT;
        double x1 = (corners[i + 1].lon - LON0) * mPerDegLon();
        double seg = hypot(x1 - x0, y1 - y0);
        int n = (int)(seg / stepM);
        if (n < 1) n = 1;
        for (int k = 0; k < n; ++k) {
            double t = (double)k / n;
            out.push_back(at(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t));
        }
    }
    out.push_back(corners.back());
    return out;
}

bool loadRoute(const std::vector<Pt>& pts) {
    std::string gpx = gpxOf(pts);
    return routes::loadFromMemory("test.gpx", gpx.c_str(), gpx.size());
}

// Convenience: what is the device announcing right now?
struct Turn {
    bool have;
    std::string instr;
    float distM;
};
Turn turnNow() {
    char buf[routes::MANEUVER_TEXT];
    float d = -1;
    bool ok = routes::nextTurn(buf, sizeof(buf), d);
    return {ok, ok ? std::string(buf) : std::string(), d};
}

void begin(const char* name) {
    g_case = name;
    routes::clearRoute();
    routes::clearManeuvers();
}

// ---------------------------------------------------------------------------

// An L: 1 km east, then 1 km north. One turn, at the corner.
void testStraightThenTurn() {
    begin("L-route: distance to the turn counts down with position");
    std::vector<Pt> corners = {at(0, 0), at(1000, 0), at(1000, 1000)};
    check(loadRoute(densify(corners, 10.0)), "route loaded");

    routes::addManeuver(at(1000, 0).lat, at(1000, 0).lon, "Turn left");
    routes::finishManeuvers();
    routes::startNav();
    check(routes::navActive(), "nav active after startNav");

    // Rider at the start: the turn is 1000 m ahead.
    routes::updateProgress(at(0, 0).lat, at(0, 0).lon);
    Turn t = turnNow();
    check(t.have, "a turn is announced at the start");
    check(t.instr == "Turn left", "correct instruction");
    checkNear(t.distM, 1000.0f, 25.0f, "distance at route start");

    // Halfway down the first leg: 500 m to go.
    routes::updateProgress(at(500, 0).lat, at(500, 0).lon);
    checkNear(turnNow().distM, 500.0f, 25.0f, "distance at 500 m along");

    // Just before the corner.
    routes::updateProgress(at(950, 0).lat, at(950, 0).lon);
    checkNear(turnNow().distM, 50.0f, 25.0f, "distance just before the turn");

    // Past the corner and up the second leg: the turn is consumed.
    routes::updateProgress(at(1000, 200).lat, at(1000, 200).lon);
    check(!turnNow().have, "turn is consumed once ridden through");
}

// Three turns in a row; each should be announced in order, never skipped.
void testTurnsAnnouncedInOrder() {
    begin("staircase: turns announced one at a time, in order");
    std::vector<Pt> corners = {at(0, 0), at(400, 0),   at(400, 400),
                               at(800, 400), at(800, 800)};
    check(loadRoute(densify(corners, 10.0)), "route loaded");
    routes::addManeuver(at(400, 0).lat,   at(400, 0).lon,   "Left onto A");
    routes::addManeuver(at(400, 400).lat, at(400, 400).lon, "Right onto B");
    routes::addManeuver(at(800, 400).lat, at(800, 400).lon, "Left onto C");
    routes::finishManeuvers();
    routes::startNav();

    struct Step { double e, n; const char* want; float dist; };
    const Step steps[] = {
        {  0,   0, "Left onto A",  400},
        {200,   0, "Left onto A",  200},
        {400, 100, "Right onto B", 300},
        {400, 300, "Right onto B", 100},
        {600, 400, "Left onto C",  200},
        {790, 400, "Left onto C",   10},
    };
    for (const Step& s : steps) {
        routes::updateProgress(at(s.e, s.n).lat, at(s.e, s.n).lon);
        Turn t = turnNow();
        char what[96];
        snprintf(what, sizeof(what), "at (%.0f,%.0f) announces '%s'", s.e, s.n, s.want);
        check(t.have && t.instr == s.want, what);
        snprintf(what, sizeof(what), "at (%.0f,%.0f) distance", s.e, s.n);
        checkNear(t.distM, s.dist, 30.0f, what);
    }
}

// THE case the monotonic snap exists for: an out-and-back. The return leg runs
// within a few metres of the outbound leg, so a nearest-point search that is not
// constrained forward will match the wrong leg and jump the turn cursor.
void testOutAndBackDoesNotSkipTurns() {
    begin("out-and-back: overlapping legs do not skip turns");
    // Out 1 km east, turn around, back 1 km, then north.
    std::vector<Pt> corners = {at(0, 0), at(1000, 0), at(1000, 8),
                               at(0, 8), at(0, 500)};
    check(loadRoute(densify(corners, 10.0)), "route loaded");
    routes::addManeuver(at(1000, 0).lat, at(1000, 0).lon, "U-turn");
    routes::addManeuver(at(0, 8).lat,    at(0, 8).lon,    "Right onto Home");
    routes::finishManeuvers();
    routes::startNav();

    // Outbound: the U-turn is ahead, NOT the later "Right onto Home" that is
    // physically 8 m away across the road.
    routes::updateProgress(at(0, 0).lat, at(0, 0).lon);
    check(turnNow().instr == "U-turn", "outbound announces the U-turn");
    routes::updateProgress(at(500, 0).lat, at(500, 0).lon);
    Turn t = turnNow();
    check(t.instr == "U-turn", "still the U-turn halfway out");
    checkNear(t.distM, 500.0f, 30.0f, "U-turn distance halfway out");

    // Returning: now the second maneuver is the live one.
    routes::updateProgress(at(1000, 8).lat, at(1000, 8).lon);
    routes::updateProgress(at(500, 8).lat, at(500, 8).lon);
    t = turnNow();
    check(t.instr == "Right onto Home", "inbound announces the second turn");
    checkNear(t.distM, 500.0f, 40.0f, "distance back to the corner");
}

// Distance must be measured ALONG the route, not straight-line. On a dogleg the
// two differ a lot, and getting this wrong is what makes a turn "not linked to
// position".
void testDistanceIsAlongRouteNotCrowFlies() {
    begin("dogleg: distance is along-route, not straight-line");
    // Out 500 m east, 500 m north, 500 m west: the end is 500 m north of the
    // start as the crow flies but 1500 m along the route.
    std::vector<Pt> corners = {at(0, 0), at(500, 0), at(500, 500), at(0, 500)};
    check(loadRoute(densify(corners, 10.0)), "route loaded");
    routes::addManeuver(at(0, 500).lat, at(0, 500).lon, "Arrive");
    routes::finishManeuvers();
    routes::startNav();

    routes::updateProgress(at(0, 0).lat, at(0, 0).lon);
    Turn t = turnNow();
    checkNear(t.distM, 1500.0f, 40.0f, "along-route distance (not the 500 m crow-flies)");
}

// GPS jitter must not advance or rewind the turn cursor.
void testJitterDoesNotAdvanceTurns() {
    begin("jitter: noise near a turn does not consume it early");
    std::vector<Pt> corners = {at(0, 0), at(1000, 0), at(1000, 500)};
    check(loadRoute(densify(corners, 10.0)), "route loaded");
    routes::addManeuver(at(1000, 0).lat, at(1000, 0).lon, "Turn left");
    routes::finishManeuvers();
    routes::startNav();

    routes::updateProgress(at(500, 0).lat, at(500, 0).lon);
    // Ten fixes of +/-8 m noise around the same spot.
    const double jitter[] = {8, -8, 5, -6, 7, -3, 4, -7, 2, -5};
    for (double j : jitter)
        routes::updateProgress(at(500 + j, j * 0.5).lat, at(500 + j, j * 0.5).lon);
    Turn t = turnNow();
    check(t.have && t.instr == "Turn left", "still announcing the same turn");
    checkNear(t.distM, 500.0f, 40.0f, "distance unchanged by jitter");
}

// Joining a route in the middle (rider starts partway along) must pick up the
// correct next turn, not replay from the beginning.
void testJoinMidRoute() {
    begin("join mid-route: picks up the next turn, not the first");
    std::vector<Pt> corners = {at(0, 0), at(400, 0), at(400, 400), at(800, 400)};
    check(loadRoute(densify(corners, 10.0)), "route loaded");
    routes::addManeuver(at(400, 0).lat,   at(400, 0).lon,   "Left onto A");
    routes::addManeuver(at(400, 400).lat, at(400, 400).lon, "Right onto B");
    routes::finishManeuvers();

    // Rider is already on the second leg when navigation starts.
    routes::updateProgress(at(400, 200).lat, at(400, 200).lon);
    routes::startNav();
    routes::updateProgress(at(400, 200).lat, at(400, 200).lon);
    Turn t = turnNow();
    check(t.have && t.instr == "Right onto B",
          "announces the upcoming turn, not the one already passed");
    checkNear(t.distM, 200.0f, 40.0f, "distance to the upcoming turn");
}

// A maneuver dropped exactly on the final point must still be reachable, and the
// route must not report a turn once everything is consumed.
void testFinalManeuverAndExhaustion() {
    begin("final maneuver: announced, then exhausted");
    std::vector<Pt> corners = {at(0, 0), at(600, 0)};
    check(loadRoute(densify(corners, 10.0)), "route loaded");
    routes::addManeuver(at(600, 0).lat, at(600, 0).lon, "Arrive at destination");
    routes::finishManeuvers();
    routes::startNav();

    routes::updateProgress(at(0, 0).lat, at(0, 0).lon);
    check(turnNow().instr == "Arrive at destination", "final maneuver announced");
    routes::updateProgress(at(599, 0).lat, at(599, 0).lon);
    check(turnNow().have, "still announced right up to the end");
    check(routes::remainingKm() < 0.05f, "route reports itself finished");
}

// Two turns close together — a slip road, or a quick left-then-right. Route
// points get decimated, so both maneuvers can snap to the SAME route index; if
// they do, they share a cumM and the cursor eats both at once, silently
// dropping a turn.
void testCloselySpacedTurns() {
    begin("closely-spaced turns: neither is silently dropped");
    // 40 m spacing between route points, turns only 25 m apart.
    std::vector<Pt> corners = {at(0, 0), at(500, 0), at(500, 25), at(900, 25)};
    check(loadRoute(densify(corners, 40.0)), "route loaded");
    routes::addManeuver(at(500, 0).lat,  at(500, 0).lon,  "Left onto Slip");
    routes::addManeuver(at(500, 25).lat, at(500, 25).lon, "Right onto Main");
    routes::finishManeuvers();
    routes::startNav();

    routes::updateProgress(at(200, 0).lat, at(200, 0).lon);
    check(turnNow().instr == "Left onto Slip", "first of the pair announced");

    // Between the two turns, the SECOND must now be live.
    routes::updateProgress(at(500, 12).lat, at(500, 12).lon);
    Turn t = turnNow();
    check(t.have, "a turn is still announced between the pair");
    check(t.instr == "Right onto Main",
          "second of the pair announced, not skipped to nothing");
}

// Routing engines place maneuvers at road-network intersections, which do not
// land exactly on a recorded/simplified GPX track. A maneuver 15-20 m off the
// line must still snap to the right place along the route.
void testManeuversSlightlyOffTrack() {
    begin("maneuvers off the track: still snap to the right along-route spot");
    std::vector<Pt> corners = {at(0, 0), at(400, 0), at(400, 400), at(800, 400)};
    check(loadRoute(densify(corners, 10.0)), "route loaded");
    // Both offset ~15 m perpendicular to the track, as a router would give.
    routes::addManeuver(at(415, 15).lat,  at(415, 15).lon,  "Left onto A");
    routes::addManeuver(at(415, 385).lat, at(415, 385).lon, "Right onto B");
    routes::finishManeuvers();
    routes::startNav();

    routes::updateProgress(at(100, 0).lat, at(100, 0).lon);
    Turn t = turnNow();
    check(t.have && t.instr == "Left onto A", "first turn announced");
    checkNear(t.distM, 300.0f, 40.0f, "distance to the off-track turn");

    routes::updateProgress(at(400, 200).lat, at(400, 200).lon);
    t = turnNow();
    check(t.have && t.instr == "Right onto B", "second turn announced after the first");
    checkNear(t.distM, 200.0f, 45.0f, "distance to the second off-track turn");
}

// Regression guard for the actual bug behind "turn-by-turn isn't linked to
// position": progress used to snap to the nearest route VERTEX, quantising the
// rider's along-route position to the point spacing. On a decimated route that
// is tens of metres, so the countdown stuck and jumped instead of tracking. Walk
// in 10 m steps along a route whose points are 50 m apart and require the
// readout to move every step.
void testDistanceTracksPositionBetweenVertices() {
    begin("sub-segment: countdown tracks position between route points");
    std::vector<Pt> corners = {at(0, 0), at(1000, 0)};
    check(loadRoute(densify(corners, 50.0)), "route loaded (50 m spacing)");
    routes::addManeuver(at(1000, 0).lat, at(1000, 0).lon, "Arrive");
    routes::finishManeuvers();
    routes::startNav();

    float prev = -1;
    for (int e = 0; e <= 400; e += 10) {
        routes::updateProgress(at(e, 0).lat, at(e, 0).lon);
        Turn t = turnNow();
        char what[96];
        snprintf(what, sizeof(what), "distance at %d m is about %d m", e, 1000 - e);
        checkNear(t.distM, (float)(1000 - e), 15.0f, what);
        if (prev >= 0) {
            snprintf(what, sizeof(what), "countdown moved between %d and %d m",
                     e - 10, e);
            check(t.distM < prev - 5.0f, what);   // strictly decreasing, not stuck
        }
        prev = t.distM;
    }
}

// A real router's polyline carries a vertex only where the road BENDS, so a
// "continue onto X" step (a name change mid-straight) has no vertex near it.
// Maneuvers used to snap to the nearest vertex, which on a sparse polyline put
// that turn hundreds of metres from where it happens — the wrong instruction
// showed at the intersection.
void testManeuverOnSparseStraight() {
    begin("sparse polyline: a maneuver mid-straight lands where it happens");
    // Corners only: three points for a 1.2 km straight and a left at the end.
    std::vector<Pt> corners = {at(0, 0), at(1200, 0), at(1200, 400)};
    check(loadRoute(corners), "route loaded (corners only)");
    check(routes::pointCount() == 3, "route really is 3 points");
    routes::addManeuver(at(600, 0).lat,  at(600, 0).lon,  "Continue onto Oak");
    routes::addManeuver(at(1200, 0).lat, at(1200, 0).lon, "Left onto Elm");
    routes::finishManeuvers();
    routes::startNav();

    routes::updateProgress(at(100, 0).lat, at(100, 0).lon);
    Turn t = turnNow();
    check(t.have && t.instr == "Continue onto Oak", "mid-straight maneuver is next");
    checkNear(t.distM, 500.0f, 20.0f, "distance to the mid-straight maneuver");

    routes::updateProgress(at(595, 0).lat, at(595, 0).lon);
    t = turnNow();
    check(t.have && t.instr == "Continue onto Oak", "still it, right up to the spot");
    checkNear(t.distM, 5.0f, 15.0f, "counts down to zero at the right place");

    routes::updateProgress(at(1150, 0).lat, at(1150, 0).lon);
    t = turnNow();
    check(t.have && t.instr == "Left onto Elm", "handed over to the next turn");
    checkNear(t.distM, 50.0f, 20.0f, "distance to the corner");
}

// THE "wrong turn at the intersection" bug: the cursor consumed a maneuver as
// soon as the projected position passed it by 5 m. Near a corner a noisy fix
// projects onto the leg AFTER the turn, so the turn was eaten while the rider
// was still short of it — and since the cursor only moves forward, the banner
// showed the NEXT turn for the whole junction.
void testNoiseNearACornerDoesNotSkipTheTurn() {
    begin("GPS noise at a corner: the turn is not consumed early");
    std::vector<Pt> corners = {at(0, 0), at(300, 0), at(300, 300), at(700, 300)};
    check(loadRoute(corners), "route loaded (corners only)");
    routes::addManeuver(at(300, 0).lat,   at(300, 0).lon,   "Left onto A");
    routes::addManeuver(at(300, 300).lat, at(300, 300).lon, "Right onto B");
    routes::finishManeuvers();
    routes::startNav();

    // Approach the corner in 5 m steps with +/-9 m of noise in both axes, the
    // sort of scatter a bike GPS produces between buildings.
    const double nx[] = {9, -7, 6, -9, 8, -4, 9, -8, 5, -9, 7, -6};
    int k = 0;
    for (double e = 240; e <= 295; e += 5) {
        double j1 = nx[k % 12], j2 = nx[(k + 5) % 12];
        k++;
        routes::updateProgress(at(e + j1, j2).lat, at(e + j1, j2).lon);
        Turn t = turnNow();
        char what[96];
        snprintf(what, sizeof(what), "at %.0f m the turn is still 'Left onto A'", e);
        check(t.have && t.instr == "Left onto A", what);
    }

    // Past the corner and clearly onto the next leg, it hands over.
    routes::updateProgress(at(300, 60).lat, at(300, 60).lon);
    Turn t = turnNow();
    check(t.have && t.instr == "Right onto B", "next turn once genuinely past");
    checkNear(t.distM, 240.0f, 30.0f, "distance to the second corner");
}

}  // namespace

int main() {
    printf("route / turn-by-turn tests\n");
    testStraightThenTurn();
    testTurnsAnnouncedInOrder();
    testOutAndBackDoesNotSkipTurns();
    testDistanceIsAlongRouteNotCrowFlies();
    testJitterDoesNotAdvanceTurns();
    testJoinMidRoute();
    testFinalManeuverAndExhaustion();
    testCloselySpacedTurns();
    testManeuversSlightlyOffTrack();
    testDistanceTracksPositionBetweenVertices();
    testManeuverOnSparseStraight();
    testNoiseNearACornerDoesNotSkipTheTurn();

    if (g_fail == 0) {
        printf("all route tests passed\n");
        return 0;
    }
    printf("%d route assertion(s) FAILED\n", g_fail);
    return 1;
}
