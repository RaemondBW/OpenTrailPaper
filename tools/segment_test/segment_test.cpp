// Host tests for live Strava segments (src/segment_match.h).
//
//  - The blob the apps send: encode -> parse round trip, every rejection,
//    and the shared fixture bytes the iOS and Android encoders are checked
//    against (kFixtureHex — keep the three copies identical).
//  - The matcher, on synthetic rides along an L-shaped segment: arming at the
//    start and setting off, progress, the live delta vs PR pace, the finish
//    time, a new PR, going off the segment (abandon), riding it the wrong
//    way, crossing the start, and the idle page's nearest segment.
//
// Geometry is built in metres around a reference point with the same flat
// approximation the firmware uses, so assertions read in metres and seconds.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "segment_match.h"

namespace {

int g_fail = 0;
const char* g_case = "";

void check(bool ok, const std::string& what) {
    if (!ok) {
        printf("  FAIL  %s: %s\n", g_case, what.c_str());
        ++g_fail;
    }
}
void checkNear(double got, double want, double tol, const std::string& what) {
    if (!(fabs(got - want) <= tol)) {
        printf("  FAIL  %s: %s (got %.2f, want %.2f +/- %.2f)\n", g_case,
               what.c_str(), got, want, tol);
        ++g_fail;
    }
}

constexpr double LAT0 = 37.83, LON0 = -122.48;
double mLon() { return 111320.0 * cos(LAT0 * M_PI / 180.0); }
struct LL { double lat, lon; };
LL at(double eastM, double northM) {
    return {LAT0 + northM / 110540.0, LON0 + eastM / mLon()};
}

// The fixtures the apps' encoders must reproduce byte for byte, v1 and v2.
// Segment 1: id 1234567890123 (past 32 bits), "Café climb" (UTF-8), PR 754 s,
// 1609.3 m, 5.7 %, three points at 10.0 / 12.5 / -3.2 m. Segment 2: id 42,
// "Flat", no PR, 500.0 m, -0.3 %, two points, no elevation.
const char* kFixtureHexV1 =
    "4f54534701000200cb04fb711f010000f2020000dd3e0000390003000b436166"
    "c3a920636c696d6258b93900401c45ffbcb939000e1c45ff52ba3900361c45ff"
    "2a000000000000000000000088130000fdff020004466c6174a0ad3900803b45"
    "ff62af3900803b45ff";
const char* kFixtureHexV2 =
    "4f54534702000200cb04fb711f010000f2020000dd3e000039000300010b4361"
    "66c3a920636c696d6258b93900401c45ff7427bcb939000e1c45ff8d2752ba39"
    "00361c45fff0262a000000000000000000000088130000fdff02000004466c61"
    "74a0ad3900803b45ff000062af3900803b45ff0000";

std::vector<uint8_t> fromHex(const char* h) {
    std::vector<uint8_t> out;
    for (size_t i = 0; h[i] && h[i + 1]; i += 2) {
        unsigned v;
        sscanf(h + i, "%2x", &v);
        out.push_back((uint8_t)v);
    }
    return out;
}
std::string toHex(const uint8_t* b, size_t n) {
    std::string s;
    char t[3];
    for (size_t i = 0; i < n; ++i) {
        snprintf(t, sizeof(t), "%02x", b[i]);
        s += t;
    }
    return s;
}

std::vector<uint8_t> fixtureBlob(int version) {
    static const int32_t lat1[] = {3783000, 3783100, 3783250};
    static const int32_t lon1[] = {-12248000, -12248050, -12248010};
    static const float elev1[] = {10.0f, 12.5f, -3.2f};
    static const int32_t lat2[] = {3780000, 3780450};
    static const int32_t lon2[] = {-12240000, -12240000};
    seg::SourceSegment src[2] = {
        {1234567890123ull, "Caf\xc3\xa9 climb", 754, 16093, 57, 3, lat1, lon1, elev1},
        {42, "Flat", 0, 5000, -3, 2, lat2, lon2},
    };
    std::vector<uint8_t> out(1024);
    out.resize(seg::encodeBlob(src, 2, out.data(), out.size(), version));
    return out;
}

// L-shaped test segment: 1000 m north from the reference point, then 500 m
// east. A vertex every 50 m.
std::vector<uint8_t> lBlob(uint32_t prSec, int extra = 0) {
    std::vector<int32_t> la, lo;
    auto add = [&](double e, double n) {
        LL p = at(e, n);
        la.push_back((int32_t)lround(p.lat * 1e5));
        lo.push_back((int32_t)lround(p.lon * 1e5));
    };
    for (int n = 0; n <= 1000; n += 50) add(0, n);
    for (int e = 50; e <= 500; e += 50) add(e, 1000);
    // A second, unrelated segment 3 km east (and optional more copies).
    std::vector<int32_t> la2, lo2;
    for (int n = 0; n <= 400; n += 100) {
        LL p = at(3000, n);
        la2.push_back((int32_t)lround(p.lat * 1e5));
        lo2.push_back((int32_t)lround(p.lon * 1e5));
    }
    std::vector<seg::SourceSegment> src;
    src.push_back({7, "The L", prSec, 15000, 20, (int)la.size(), la.data(), lo.data()});
    src.push_back({8, "East one", 60, 4000, 0, (int)la2.size(), la2.data(), lo2.data()});
    for (int i = 0; i < extra; ++i)
        src.push_back({(uint64_t)(100 + i), "Copy", 60, 4000, 0, (int)la2.size(),
                       la2.data(), lo2.data()});
    std::vector<uint8_t> out(seg::BLOB_MAX + 1024);
    out.resize(seg::encodeBlob(src.data(), (int)src.size(), out.data(), out.size()));
    return out;
}

// A rider: positions in metres, one fix a second from t (ms).
struct Rider {
    seg::Matcher& m;
    uint32_t t;
    double e, n;
    void fix() {
        LL p = at(e, n);
        m.update(t, p.lat, p.lon);
    }
    // Ride in a straight line to (e2,n2) at `mps`, one fix a second.
    void rideTo(double e2, double n2, double mps) {
        const double de = e2 - e, dn = n2 - n;
        const double len = sqrt(de * de + dn * dn);
        const int steps = (int)ceil(len / mps);
        const double e0 = e, n0 = n;
        for (int i = 1; i <= steps; ++i) {
            const double f = std::min(1.0, i * mps / len);
            e = e0 + de * f;
            n = n0 + dn * f;
            t += 1000;
            fix();
        }
    }
    void wait(int sec) {
        for (int i = 0; i < sec; ++i) {
            t += 1000;
            fix();
        }
    }
};

void testBlob() {
    g_case = "blob";
    for (int ver = 1; ver <= 2; ++ver) {
        const char* want = ver == 1 ? kFixtureHexV1 : kFixtureHexV2;
        std::vector<uint8_t> enc = fixtureBlob(ver);
        const std::string hex = toHex(enc.data(), enc.size());
        check(hex == want, "v" + std::to_string(ver) + " fixture bytes: " + hex);
        // Parse the shared bytes themselves: what the apps send.
        std::vector<uint8_t> b = fromHex(want);
        seg::SegmentDb db;
        const char* err = "";
        check(seg::parseBlob(b.data(), b.size(), db, &err), std::string("parse: ") + err);
        check(db.count == 2, "count");
        if (db.count != 2) continue;
        check(db.segs[0].id == 1234567890123ull, "id past 32 bits");
        check(strcmp(db.segs[0].name, "Caf\xc3\xa9 climb") == 0, "utf-8 name");
        check(db.segs[0].prSec == 754 && db.segs[0].distDm == 16093 &&
                  db.segs[0].gradeD == 57,
              "pr / distance / grade");
        check(db.segs[1].gradeD == -3 && db.segs[1].prSec == 0, "negative grade, no PR");
        checkNear(db.segs[1].lenM, 497.4, 1.0, "length from the points");
        checkNear(db.segs[1].startBearingDeg, 0, 0.5, "due north");
        check(db.segs[1].nPts == 2 && db.totalPts == 5, "points");
        check(!db.segs[1].hasElev, "segment 2 has no elevation");
        if (ver == 1) {
            check(!db.segs[0].hasElev, "v1 carries no elevation");
        } else {
            check(db.segs[0].hasElev, "v2 elevation");
            checkNear(db.elev[0], 10.0, 0.05, "elev 0");
            checkNear(db.elev[1], 12.5, 0.05, "elev 1");
            checkNear(db.elev[2], -3.2, 0.05, "elev 2 below sea level");
            checkNear(db.segs[0].minElevM, -3.2, 0.05, "min elev");
            checkNear(db.segs[0].maxElevM, 12.5, 0.05, "max elev");
            const seg::Segment& s0 = db.segs[0];
            checkNear(seg::elevAt(db, s0, db.cum[1] / 2), 11.25, 0.05, "elevation between points");
            checkNear(seg::elevAt(db, s0, 1e6), -3.2, 0.05, "past the end");
        }
    }
    std::vector<uint8_t> b = fromHex(kFixtureHexV2);
    seg::SegmentDb db;
    // The empty set is valid (the rider unstarred everything).
    const uint8_t empty[] = {'O', 'T', 'S', 'G', 1, 0, 0, 0};
    check(seg::parseBlob(empty, sizeof(empty), db) && db.count == 0, "empty set");

    // Rejections leave the old set alone.
    seg::SegmentDb keep;
    seg::parseBlob(b.data(), b.size(), keep);
    auto rejects = [&](std::vector<uint8_t> bad, const char* what) {
        check(!seg::parseBlob(bad.data(), bad.size(), keep), what);
        check(keep.count == 2, std::string(what) + " keeps the old set");
    };
    std::vector<uint8_t> bad = b;
    bad[0] = 'X';
    rejects(bad, "bad magic");
    bad = b;
    bad[4] = 3;
    rejects(bad, "a version from the future");
    bad = b;
    bad[4] = 0;
    rejects(bad, "version 0");
    bad = b;
    bad[4] = 1;   // v2 bytes labelled v1: the sizes no longer add up
    rejects(bad, "v2 body as v1");
    rejects(std::vector<uint8_t>(b.begin(), b.end() - 1), "truncated");
    bad = b;
    bad.push_back(0);
    rejects(bad, "trailing byte");
    bad = b;
    bad[8 + 18] = 1;   // segment 1 nPts = 1
    bad[8 + 19] = 0;
    rejects(bad, "one point");
    bad = b;
    bad[6] = 3;        // count says three
    rejects(bad, "count past the data");
    rejects(std::vector<uint8_t>(b.begin(), b.begin() + 5), "header only");

    // Caps: MAX_SEGMENTS fits, one more does not encode.
    std::vector<uint8_t> big = lBlob(300, seg::MAX_SEGMENTS - 2);
    check(!big.empty() && big.size() <= seg::BLOB_MAX, "64 segments fit BLOB_MAX");
    check(seg::parseBlob(big.data(), big.size(), db) && db.count == seg::MAX_SEGMENTS,
          "64 segments parse");
    check(lBlob(300, seg::MAX_SEGMENTS - 1).empty(), "65 refused");
}

// Ride the whole L at `mps` from 100 m south, then on 100 m past the end.
struct Effort {
    seg::View mid;      // view at the halfway mark
    seg::View done;     // just after the finish
    uint32_t startT = 0;
    bool started = false, ended = false;
};
Effort rideL(seg::Matcher& m, double mps, double wobbleM = 0) {
    Effort r;
    Rider rd{m, 1000000, 0, -100};
    rd.fix();
    // Approach to the start line.
    rd.rideTo(wobbleM, -5, mps);
    r.startT = rd.t + (uint32_t)(5 / mps * 1000);
    rd.rideTo(-wobbleM, 750, mps);
    r.started = m.takeStarted();
    m.view(rd.t, r.mid);
    rd.rideTo(0, 1000, mps);
    rd.rideTo(600, 1000, mps);
    r.ended = m.takeEnded();
    m.view(rd.t, r.done);
    return r;
}

void testEffort() {
    g_case = "effort at PR pace";
    std::vector<uint8_t> b = lBlob(300);   // 1500 m in 300 s = 5 m/s
    seg::SegmentDb db;
    seg::parseBlob(b.data(), b.size(), db);
    checkNear(db.segs[0].lenM, 1500, 2, "L length");
    seg::Matcher m;
    m.attach(&db);
    Effort e = rideL(m, 5.0);
    check(e.started, "started");
    check(e.mid.phase == seg::ACTIVE, "active at 750 m north");
    check(strcmp(e.mid.name, "The L") == 0, "name");
    checkNear(e.mid.doneM, 750, 6, "progress");
    // What the page's map draws: this segment, the rider in its frame.
    check(e.mid.db == &db && e.mid.segIndex == 0 && e.mid.riderValid, "map segment + rider");
    checkNear(e.mid.riderX, 0, 1, "rider east of the start");
    checkNear(e.mid.riderY, 750, 2, "rider north of the start");
    check(e.done.segIndex == 0, "result keeps its segment for the map");
    checkNear(e.mid.lenM, 1500, 2, "length");
    check(e.mid.haveDelta, "delta");
    checkNear(e.mid.deltaMs / 1000.0, 0, 2.0, "on PR pace");
    check(e.ended && e.done.phase == seg::DONE, "finished");
    checkNear(e.done.elapsedMs / 1000.0, 300, 2.0, "finish time");
    checkNear(e.done.deltaMs / 1000.0, 0, 2.0, "result vs PR");
    check(e.done.prSec == 300, "result shows the PR it was against");

    g_case = "effort faster than PR";
    seg::SegmentDb db2;
    seg::parseBlob(b.data(), b.size(), db2);
    seg::Matcher f;
    f.attach(&db2);
    Effort fast = rideL(f, 6.0, 8);   // 250 s, weaving 8 m either side
    check(fast.mid.phase == seg::ACTIVE, "active");
    // At 750 m: 125 s ridden vs 150 s at PR pace.
    checkNear(fast.mid.deltaMs / 1000.0, -25, 2.5, "ahead mid-way");
    check(fast.done.phase == seg::DONE, "finished");
    checkNear(fast.done.elapsedMs / 1000.0, 250, 2.0, "finish time");
    checkNear(fast.done.deltaMs / 1000.0, -50, 2.0, "50 s under");
    check(fast.done.newPr, "new PR");
    check(db2.segs[0].prSec == 250, "device keeps the new PR");
    // The result lapses back to idle.
    check(f.phase() == seg::DONE, "result up");
    f.tick(1000000 + 400 * 1000 + seg::RESULT_MS);
    check(f.phase() == seg::IDLE, "result lapses");

    g_case = "effort slower, no PR on file";
    std::vector<uint8_t> nb = lBlob(0);
    seg::SegmentDb db3;
    seg::parseBlob(nb.data(), nb.size(), db3);
    seg::Matcher s;
    s.attach(&db3);
    Effort slow = rideL(s, 4.0);
    check(!slow.mid.haveDelta, "no delta without a PR");
    check(slow.done.phase == seg::DONE && slow.done.newPr, "first effort is a PR");
    checkNear(slow.done.elapsedMs / 1000.0, 375, 2.0, "finish time");
}

void testStartTiming() {
    g_case = "waiting at the start";
    std::vector<uint8_t> b = lBlob(300);
    seg::SegmentDb db;
    seg::parseBlob(b.data(), b.size(), db);
    seg::Matcher m;
    m.attach(&db);
    Rider rd{m, 1000000, 0, -100};
    rd.fix();
    rd.rideTo(0, -2, 5);
    check(m.phase() == seg::ARMED, "armed at the line");
    rd.wait(30);   // a red light at the start
    const uint32_t go = rd.t;
    rd.rideTo(0, 500, 5);
    seg::View v;
    m.view(rd.t, v);
    check(v.phase == seg::ACTIVE, "active");
    // 502 m at 5 m/s from the moment of setting off, not from arriving.
    checkNear(v.elapsedMs / 1000.0, (rd.t - go) / 1000.0, 1.5, "clock from the off");
}

void testAbandon() {
    g_case = "off the segment";
    std::vector<uint8_t> b = lBlob(300);
    seg::SegmentDb db;
    seg::parseBlob(b.data(), b.size(), db);
    seg::Matcher m;
    m.attach(&db);
    Rider rd{m, 1000000, 0, -100};
    rd.fix();
    rd.rideTo(0, 400, 5);
    check(m.phase() == seg::ACTIVE && m.takeStarted(), "active");
    // A short detour (10 s, 70 m out) is forgiven...
    rd.rideTo(70, 425, 5);
    rd.rideTo(0, 450, 5);
    rd.rideTo(0, 500, 5);
    check(m.phase() == seg::ACTIVE, "short detour forgiven");
    seg::View v;
    m.view(rd.t, v);
    check(!v.offRoute, "back on");
    // ...a turn off down another road is not.
    // (36 s east: off from ~50 m out, abandoned 20 s later, the note up 10 s.)
    rd.rideTo(180, 500, 5);
    m.view(rd.t, v);
    check(m.phase() == seg::ABANDONED && m.takeEnded(), "abandoned");
    check(v.phase == seg::ABANDONED && strcmp(v.name, "The L") == 0, "says which");
    // The note lapses; riding the rest doesn't resurrect it.
    rd.rideTo(300, 1000, 5);
    rd.wait(10);
    check(m.phase() == seg::IDLE, "idle after the note");
}

void testWrongWay() {
    g_case = "ridden backwards";
    std::vector<uint8_t> b = lBlob(300);
    seg::SegmentDb db;
    seg::parseBlob(b.data(), b.size(), db);
    seg::Matcher m;
    m.attach(&db);
    // From the far end back to the start and beyond, southbound.
    Rider rd{m, 1000000, 600, 1000};
    rd.fix();
    rd.rideTo(0, 1000, 5);
    rd.rideTo(0, -200, 5);
    check(!m.takeStarted() && m.phase() == seg::IDLE, "never started");

    g_case = "crossing the start";
    seg::Matcher c;
    c.attach(&db);
    Rider x{c, 1000000, -200, 3};
    x.fix();
    x.rideTo(200, 3, 5);
    check(!c.takeStarted() && c.phase() == seg::IDLE, "across the line: no effort");
}

void testNearest() {
    g_case = "nearest";
    std::vector<uint8_t> b = lBlob(300);
    seg::SegmentDb db;
    seg::parseBlob(b.data(), b.size(), db);
    seg::Matcher m;
    m.attach(&db);
    Rider rd{m, 1000000, 0, -2000};
    rd.fix();
    seg::View v;
    m.view(rd.t, v);
    check(v.count == 2, "count");
    check(v.haveNearest && strcmp(v.nearestName, "The L") == 0, "the L is nearest");
    check(v.segIndex == 0 && v.riderValid, "idle map: the nearest segment");
    checkNear(v.riderY, -2000, 3, "rider 2 km south of its start");
    checkNear(v.nearestM, 2000, 5, "2 km to its start");
    checkNear(v.nearestBearingDeg, 0, 1, "due north of the rider");
    check(v.nearestPrSec == 300, "its PR");
    // Over by the east one.
    Rider e{m, 2000000, 3000, -500};
    e.fix();
    m.view(e.t, v);
    check(v.haveNearest && strcmp(v.nearestName, "East one") == 0, "east one");
    // Far from everything.
    Rider far{m, 3000000, 0, -20000};
    far.fix();
    m.view(far.t, v);
    check(!v.haveNearest, "nothing within 5 km");
    // No segments at all.
    seg::SegmentDb none;
    seg::Matcher z;
    z.attach(&none);
    Rider n{z, 1000, 0, 0};
    n.fix();
    z.view(n.t, v);
    check(v.count == 0 && !v.haveNearest && v.phase == seg::IDLE, "no segments");
}

void testLoop() {
    g_case = "loop segment";
    // A 400 x 400 m square that ends where it starts.
    std::vector<int32_t> la, lo;
    auto add = [&](double e, double n) {
        LL p = at(e, n);
        la.push_back((int32_t)lround(p.lat * 1e5));
        lo.push_back((int32_t)lround(p.lon * 1e5));
    };
    for (int n = 0; n <= 400; n += 50) add(0, n);
    for (int e = 50; e <= 400; e += 50) add(e, 400);
    for (int n = 350; n >= 0; n -= 50) add(400, n);
    for (int e = 350; e >= 0; e -= 50) add(e, 0);
    seg::SourceSegment src = {9, "Square", 0, 16000, 0, (int)la.size(), la.data(), lo.data()};
    std::vector<uint8_t> b(8192);
    b.resize(seg::encodeBlob(&src, 1, b.data(), b.size()));
    seg::SegmentDb db;
    check(seg::parseBlob(b.data(), b.size(), db), "parse");
    seg::Matcher m;
    m.attach(&db);
    Rider rd{m, 1000000, 0, -50};
    rd.fix();
    rd.rideTo(0, 400, 5);
    rd.rideTo(400, 400, 5);
    rd.rideTo(400, 0, 5);
    check(m.phase() == seg::ACTIVE, "not finished at 3/4 (start == end)");
    rd.rideTo(0, 0, 5);
    rd.rideTo(-60, 0, 5);
    seg::View v;
    m.view(rd.t, v);
    check(v.phase == seg::DONE, "finished on closing the loop");
    checkNear(v.elapsedMs / 1000.0, 1600 / 5.0, 2.0, "lap time");
}

}  // namespace

int main() {
    testBlob();
    testEffort();
    testStartTiming();
    testAbandon();
    testWrongWay();
    testNearest();
    testLoop();
    if (g_fail) {
        printf("segment tests: %d FAILED\n", g_fail);
        return 1;
    }
    printf("segment tests passed\n");
    return 0;
}
