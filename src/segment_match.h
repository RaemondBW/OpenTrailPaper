#pragma once

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

// Live Strava segments: the rider's STARRED segments, synced by the app, and
// the matcher that times them on the SEGMENT page (`page segment`).
//
// The app does all the Strava work (the API, the PR times, cutting each
// segment's line down to MAX_POINTS) and sends the device one compact blob
// over BLE. The device keeps it on the SD card as /segments/starred.bin and
// parses it into a SegmentDb. Everything in this header is host-safe — no
// Arduino, `now` is passed in (millis()) — so tools/segment_test covers the
// format and the matcher on a synthetic ride.
//
// --- Blob format (little-endian) ------------------------------------------
//   header   "OTSG"  u8 version (1 or 2)  u8 flags (0)  u16 count
//   per segment, `count` times:
//     u64 id              Strava segment id
//     u32 prSec           the athlete's PR elapsed time, s (0 = none)
//     u32 distDm          Strava's segment distance, decimetres
//     i16 gradeD          average grade, 0.1 %
//     u16 nPts            points, 2..MAX_POINTS
//     u8  segFlags        v2 only: bit 0 = the points carry real elevation
//     u8  nameLen         UTF-8 bytes, <= NAME_BYTES
//     name bytes
//     nPts x (i32 latE5, i32 lonE5)            v1: 8 bytes a point
//     nPts x (i32 latE5, i32 lonE5, u16 elev)  v2: 10 bytes a point;
//                         elev = (metres + ELEV_OFFSET_M) x 10, i.e.
//                         decimetres from -1000 m to +5553 m
// Version 2 adds the elevation for the page's profile. Firmware reads both;
// the apps send v2 only to a device that said it takes it (the count query's
// reply carries the newest version, see ble_server.cpp). An unknown version
// is rejected like any other malformed blob: the whole thing, so a
// half-received or too-new sync never replaces a good set. The apps' encoders
// live in companion-ios/Sources/StravaSegments.swift and
// companion-android/.../data/StravaSegments.kt; all three are checked against
// the same bytes (tools/segment_test).
//
// --- The matcher ----------------------------------------------------------
// Fed each GPS fix (position + the millis() it arrived). Phases:
//   IDLE      scanning: the nearest segment start for the idle page, and
//             whether the rider is at a start heading along the first leg.
//   ARMED     within ARM_RADIUS_M of a start. It becomes an effort once the
//             rider sets off along the first leg, heading within
//             ARM_HEADING_DEG of it; leaving the circle any other way
//             disarms. The effort's clock starts at the fix CLOSEST to the start (the
//             latest one on a tie, so waiting at the line doesn't count), once
//             the rider is clearly under way along it.
//   ACTIVE    progress = the rider projected onto the polyline, searched in a
//             window around the last match so a segment that doubles back on
//             itself can't jump ahead. More than OFF_ROUTE_M off it for
//             OFF_ROUTE_MS abandons the effort. Near the end (and most of the
//             way along) the finish is the fix closest to the end point.
//   DONE      the result (time, delta vs PR), shown for RESULT_MS.
//   ABANDONED a short "off segment" note, shown for NOTE_MS.
// The last two are only what the page shows: underneath, the matcher is
// already scanning (or armed at the next start) again.
// The live delta compares against PR pace: the PR's time scaled by the
// fraction of the segment done (constant-pace; Strava keeps no splits).
// Efforts are tracked independently, up to MAX_EFFORTS at once, so starred
// segments that overlap are all timed. list() orders what the page pages
// through: efforts under way (and results on show) by start time, then the
// starts the rider is at or near, by distance.

namespace seg {

constexpr uint8_t BLOB_VERSION_MIN = 1;
constexpr uint8_t BLOB_VERSION = 2;   // the newest this firmware reads
constexpr int MAX_SEGMENTS = 64;
constexpr int MAX_POINTS = 200;
constexpr int NAME_BYTES = 47;
constexpr int ELEV_OFFSET_M = 1000;
constexpr size_t HEADER_BYTES = 8;
constexpr size_t SEG_FIXED_BYTES_V1 = 8 + 4 + 4 + 2 + 2 + 1;
constexpr size_t SEG_FIXED_BYTES_V2 = SEG_FIXED_BYTES_V1 + 1;
constexpr size_t BLOB_MAX =
    HEADER_BYTES + (size_t)MAX_SEGMENTS * (SEG_FIXED_BYTES_V2 + NAME_BYTES + MAX_POINTS * 10);
inline size_t segFixedBytes(int version) {
    return version >= 2 ? SEG_FIXED_BYTES_V2 : SEG_FIXED_BYTES_V1;
}
inline size_t pointBytes(int version) { return version >= 2 ? 10 : 8; }

// Matcher tuning.
constexpr float ARM_RADIUS_M = 25.0f;     // at the start
constexpr float ARM_HEADING_DEG = 60.0f;  // vs the first leg's bearing
constexpr float GO_ALONG_M = 10.0f;       // under way past the start
constexpr float OFF_ROUTE_M = 50.0f;
constexpr uint32_t OFF_ROUTE_MS = 20000;
constexpr float FINISH_RADIUS_M = 25.0f;
constexpr float FINISH_MIN_FRACTION = 0.8f;
constexpr uint32_t FINISH_SETTLE_MS = 5000;   // stopped at the line
constexpr uint32_t RESULT_MS = 30000;
constexpr uint32_t NOTE_MS = 10000;
constexpr float NEARBY_M = 5000.0f;        // the idle page's "next" segment
constexpr float WINDOW_BACK_M = 60.0f;
constexpr float WINDOW_AHEAD_M = 400.0f;
constexpr float COURSE_MIN_MOVE_M = 5.0f;  // displacement for a heading

constexpr double M_PER_DEG_LAT = 110540.0;
constexpr double M_PER_DEG_LON_EQ = 111320.0;

struct Segment {
    uint64_t id = 0;
    char name[NAME_BYTES + 1] = "";
    uint32_t prSec = 0;       // 0 = no PR
    uint32_t distDm = 0;      // Strava's distance (display)
    int16_t gradeD = 0;
    int first = 0;            // index of point 0 in SegmentDb's arrays
    int nPts = 0;
    float lenM = 0;           // along the polyline as stored
    double lat0 = 0, lon0 = 0;  // point 0: the local frame's origin
    float mPerDegLon = 0;
    float startBearingDeg = 0;
    // Bounding box, degrees, for a quick reject.
    double minLat = 0, maxLat = 0, minLon = 0, maxLon = 0;
    // The same box in the local frame (metres), for drawing the line.
    float minX = 0, maxX = 0, minY = 0, maxY = 0;
    // Elevation along it (v2 blobs that carried it); else no profile.
    bool hasElev = false;
    float minElevM = 0, maxElevM = 0;
    float gainM = 0;          // total climbing along it
};

inline void* segAlloc(size_t n) {
#ifdef ARDUINO
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
#else
    return malloc(n);
#endif
}

// The parsed set. Points are in each segment's local metric frame (metres
// east/north of its point 0), with the along-polyline distance to each and
// the elevation (0 where the segment has none). 16 bytes a point: the full
// 64 x 200 set is 205 KB, in PSRAM on the device.
struct SegmentDb {
    int count = 0;
    Segment* segs = nullptr;
    float* x = nullptr;
    float* y = nullptr;
    float* cum = nullptr;
    float* elev = nullptr;
    int totalPts = 0;

    SegmentDb() = default;
    SegmentDb(const SegmentDb&) = delete;
    SegmentDb& operator=(const SegmentDb&) = delete;
    ~SegmentDb() { clear(); }

    void clear() {
        free(segs);
        free(x);
        free(y);
        free(cum);
        free(elev);
        segs = nullptr;
        x = y = cum = elev = nullptr;
        count = totalPts = 0;
    }
    void swap(SegmentDb& o) {
        int c = count; count = o.count; o.count = c;
        int t = totalPts; totalPts = o.totalPts; o.totalPts = t;
        Segment* s = segs; segs = o.segs; o.segs = s;
        float* f;
        f = x; x = o.x; o.x = f;
        f = y; y = o.y; o.y = f;
        f = cum; cum = o.cum; o.cum = f;
        f = elev; elev = o.elev; o.elev = f;
    }
};

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}
inline uint64_t rd64(const uint8_t* p) {
    return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

inline float bearingDeg(float dx, float dy) {
    float b = atan2f(dx, dy) * 57.2957795f;
    return b < 0 ? b + 360.0f : b;
}
inline float angleDiff(float a, float b) {
    float d = fabsf(a - b);
    while (d > 360.0f) d -= 360.0f;
    return d > 180.0f ? 360.0f - d : d;
}

// Walks the blob once to validate it and count points. Returns the segment
// count, or -1 (with `err`) when anything is off.
inline int scanBlob(const uint8_t* b, size_t n, int* totalPts, const char** err) {
    auto fail = [&](const char* m) { if (err) *err = m; return -1; };
    if (!b || n < HEADER_BYTES) return fail("too short");
    if (memcmp(b, "OTSG", 4) != 0) return fail("bad magic");
    const int ver = b[4];
    if (ver < BLOB_VERSION_MIN || ver > BLOB_VERSION) return fail("unknown version");
    const size_t fixed = segFixedBytes(ver), ptB = pointBytes(ver);
    const int count = rd16(b + 6);
    if (count > MAX_SEGMENTS) return fail("too many segments");
    size_t off = HEADER_BYTES;
    int pts = 0;
    for (int i = 0; i < count; ++i) {
        if (n - off < fixed) return fail("truncated segment");
        const int np = rd16(b + off + 18);
        const int nl = b[off + fixed - 1];
        if (np < 2 || np > MAX_POINTS) return fail("bad point count");
        if (nl > NAME_BYTES) return fail("name too long");
        const size_t need = fixed + nl + (size_t)np * ptB;
        if (n - off < need) return fail("truncated points");
        off += need;
        pts += np;
    }
    if (off != n) return fail("trailing bytes");
    if (totalPts) *totalPts = pts;
    return count;
}

// Parses a blob into `out` (replacing what it held). False = rejected, `out`
// untouched.
inline bool parseBlob(const uint8_t* b, size_t n, SegmentDb& out,
                      const char** err = nullptr) {
    int total = 0;
    const int count = scanBlob(b, n, &total, err);
    if (count < 0) return false;
    const int ver = b[4];
    const size_t fixed = segFixedBytes(ver), ptB = pointBytes(ver);
    SegmentDb db;
    db.count = count;
    db.totalPts = total;
    if (count > 0) {
        db.segs = (Segment*)segAlloc(sizeof(Segment) * count);
        db.x = (float*)segAlloc(sizeof(float) * total);
        db.y = (float*)segAlloc(sizeof(float) * total);
        db.cum = (float*)segAlloc(sizeof(float) * total);
        db.elev = (float*)segAlloc(sizeof(float) * total);
        if (!db.segs || !db.x || !db.y || !db.cum || !db.elev) {
            if (err) *err = "out of memory";
            return false;
        }
    }
    size_t off = HEADER_BYTES;
    int at = 0;
    for (int i = 0; i < count; ++i) {
        Segment& s = db.segs[i];
        s = Segment();
        const uint8_t* p = b + off;
        s.id = rd64(p);
        s.prSec = rd32(p + 8);
        s.distDm = rd32(p + 12);
        s.gradeD = (int16_t)rd16(p + 16);
        s.nPts = rd16(p + 18);
        s.hasElev = ver >= 2 && (p[20] & 1);
        const int nl = p[fixed - 1];
        memcpy(s.name, p + fixed, nl);
        s.name[nl] = 0;
        const uint8_t* q = p + fixed + nl;
        s.first = at;
        s.lat0 = (int32_t)rd32(q) / 1e5;
        s.lon0 = (int32_t)rd32(q + 4) / 1e5;
        s.mPerDegLon = (float)(M_PER_DEG_LON_EQ * cos(s.lat0 * M_PI / 180.0));
        s.minLat = s.maxLat = s.lat0;
        s.minLon = s.maxLon = s.lon0;
        float len = 0;
        bool haveBearing = false;
        for (int k = 0; k < s.nPts; ++k) {
            const uint8_t* r = q + k * ptB;
            const double lat = (int32_t)rd32(r) / 1e5;
            const double lon = (int32_t)rd32(r + 4) / 1e5;
            const float px = (float)((lon - s.lon0) * s.mPerDegLon);
            const float py = (float)((lat - s.lat0) * M_PER_DEG_LAT);
            if (k > 0) {
                const float dx = px - db.x[at + k - 1], dy = py - db.y[at + k - 1];
                len += sqrtf(dx * dx + dy * dy);
            }
            db.x[at + k] = px;
            db.y[at + k] = py;
            db.cum[at + k] = len;
            const float e = s.hasElev ? rd16(r + 8) / 10.0f - ELEV_OFFSET_M : 0.0f;
            db.elev[at + k] = e;
            if (k > 0 && e > db.elev[at + k - 1]) s.gainM += e - db.elev[at + k - 1];
            if (k == 0 || e < s.minElevM) s.minElevM = e;
            if (k == 0 || e > s.maxElevM) s.maxElevM = e;
            // The first leg's direction: from the start to the first point at
            // least 15 m out (a polyline can open with a 1 m jitter).
            if (!haveBearing && (sqrtf(px * px + py * py) >= 15.0f || k == s.nPts - 1) && k > 0) {
                s.startBearingDeg = bearingDeg(px, py);
                haveBearing = true;
            }
            if (lat < s.minLat) s.minLat = lat;
            if (lat > s.maxLat) s.maxLat = lat;
            if (lon < s.minLon) s.minLon = lon;
            if (lon > s.maxLon) s.maxLon = lon;
            if (k == 0 || px < s.minX) s.minX = px;
            if (k == 0 || px > s.maxX) s.maxX = px;
            if (k == 0 || py < s.minY) s.minY = py;
            if (k == 0 || py > s.maxY) s.maxY = py;
        }
        s.lenM = len;
        at += s.nPts;
        off += fixed + nl + (size_t)s.nPts * ptB;
    }
    out.swap(db);
    return true;
}

// Elevation at `alongM` metres along segment `s` (linear between points).
inline float elevAt(const SegmentDb& db, const Segment& s, float alongM) {
    const float* C = db.cum + s.first;
    const float* E = db.elev + s.first;
    if (alongM <= 0) return E[0];
    for (int i = 0; i + 1 < s.nPts; ++i) {
        if (alongM <= C[i + 1]) {
            const float span = C[i + 1] - C[i];
            const float t = span > 0 ? (alongM - C[i]) / span : 0;
            return E[i] + t * (E[i + 1] - E[i]);
        }
    }
    return E[s.nPts - 1];
}

enum Phase : uint8_t { IDLE, ARMED, ACTIVE, DONE, ABANDONED };

// What the SEGMENT page draws for one segment. Filled by Matcher::view().
struct View {
    int count = 0;              // segments on the device
    Phase phase = IDLE;
    // IDLE / ARMED: the segment's start, from the rider.
    bool haveNearest = false;
    char nearestName[NAME_BYTES + 1] = "";
    float nearestM = 0;
    uint32_t nearestPrSec = 0;
    float nearestLenM = 0;
    int16_t nearestGradeD = 0;
    float nearestBearingDeg = 0;   // from the rider to the start
    // ARMED / ACTIVE / DONE / ABANDONED: the segment in question.
    char name[NAME_BYTES + 1] = "";
    uint32_t prSec = 0;
    float lenM = 0;
    int16_t gradeD = 0;
    float doneM = 0;
    uint32_t elapsedMs = 0;
    bool haveDelta = false;
    int32_t deltaMs = 0;        // + behind PR pace, - ahead
    bool newPr = false;         // DONE: beat (or set) the PR
    bool offRoute = false;      // ACTIVE: currently off the line
    // DONE: the effort's averages over its timed span (sample()); 0xFFFF / 0
    // = that sensor never reported during it.
    uint16_t avgPowerW = 0xFFFF;
    uint8_t avgHrBpm = 0;
    // The segment the page draws (map + profile). -1 = none. `db` stays
    // valid for the frame (the UI task owns both).
    const SegmentDb* db = nullptr;
    int segIndex = -1;
    // The rider in that segment's local frame (metres east/north of its start).
    bool riderValid = false;
    float riderX = 0, riderY = 0;
    // Which of the relevant segments this is (Matcher::list), for the page's
    // arrows: 1-based position and how many there are. Set by the caller.
    int listPos = 0, listCount = 0;
};

// How many of each the matcher keeps at once.
constexpr int MAX_EFFORTS = 4;    // armed + under way, overlapping segments
constexpr int MAX_RESULTS = 4;    // finished / abandoned, still on show
constexpr int MAX_UPCOMING = 3;   // nearest starts within NEARBY_M
constexpr int MAX_LIST = MAX_EFFORTS + MAX_RESULTS + MAX_UPCOMING;

class Matcher {
public:
    // The set to match against. The matcher never owns it; call again
    // whenever the caller swaps in a new one (it starts over).
    void attach(SegmentDb* db) {
        db_ = db;
        reset();
    }
    void reset() {
        for (Effort& e : eff_) e = Effort();
        for (Result& r : res_) r = Result();
        upCount_ = 0;
        haveCourse_ = false;
        haveAnchor_ = false;
        haveRider_ = false;
        started_ = ended_ = false;
        startedSeg_ = -1;
    }

    // The segments that matter now, in the order the page pages through them:
    // efforts under way and results still on show, by when they started; then
    // starts the rider is at (armed) or near, by distance. One entry per
    // segment (an effort under way outranks its own result, a result its own
    // re-armed start). Returns how many; `out` gets segment indices.
    int list(uint32_t now, int* out, int cap) const {
        struct Item { int seg; int group; float key; };
        Item items[MAX_LIST];
        int n = 0;
        auto has = [&](int seg) {
            for (int i = 0; i < n; ++i) if (items[i].seg == seg) return true;
            return false;
        };
        for (const Effort& e : eff_)
            if (e.used && e.phase == ACTIVE && n < MAX_LIST)
                items[n++] = {e.seg, 0, (float)(int32_t)(e.startMs - epoch_)};
        for (const Result& r : res_)
            if (showing(r, now) && !has(r.seg) && n < MAX_LIST)
                items[n++] = {r.seg, 0, (float)(int32_t)(r.startMs - epoch_)};
        for (const Effort& e : eff_)
            if (e.used && e.phase == ARMED && !has(e.seg) && n < MAX_LIST)
                items[n++] = {e.seg, 1, e.startDistM};
        for (int i = 0; i < upCount_; ++i)
            if (!has(upSeg_[i]) && n < MAX_LIST) items[n++] = {upSeg_[i], 1, upM_[i]};
        // Insertion sort: a handful of items.
        for (int i = 1; i < n; ++i)
            for (int j = i; j > 0; --j) {
                const Item& a = items[j - 1];
                const Item& b = items[j];
                if (a.group < b.group || (a.group == b.group && a.key <= b.key)) break;
                const Item t = items[j - 1];
                items[j - 1] = items[j];
                items[j] = t;
            }
        int k = 0;
        for (int i = 0; i < n && k < cap; ++i) out[k++] = items[i].seg;
        return k;
    }

    // The first of list(): what a single-segment page shows.
    int primary(uint32_t now) const {
        int segs[MAX_LIST];
        return list(now, segs, MAX_LIST) > 0 ? segs[0] : -1;
    }

    // The primary segment's phase (IDLE when there is none).
    Phase phase() const { return phaseOf(primary(lastNow_), lastNow_); }

    // An effort began / ended since the last call (for the page auto-switch);
    // `seg` gets the segment that started last.
    bool takeStarted(int* seg = nullptr) {
        const bool v = started_;
        started_ = false;
        if (seg) *seg = startedSeg_;
        return v;
    }
    bool takeEnded() { bool v = ended_; ended_ = false; return v; }
    // An effort is being timed: the device must not auto-sleep.
    bool busy() const {
        for (const Effort& e : eff_) if (e.used && e.phase == ACTIVE) return true;
        return false;
    }

    // Time passes with no new fix: settles a finish where the rider stopped
    // on the line, lapses results. Returns `now`, never earlier than a time
    // already seen (a fix stamped a moment before the last tick must not run
    // the clock backwards).
    uint32_t tick(uint32_t now) {
        if (haveNow_ && (int32_t)(now - lastNow_) < 0) now = lastNow_;
        if (!haveNow_) epoch_ = now;
        lastNow_ = now;
        haveNow_ = true;
        for (Effort& e : eff_)
            if (e.used && e.phase == ACTIVE && e.finishing &&
                now - e.bestEndMs >= FINISH_SETTLE_MS)
                finish(e, now);
        for (Result& r : res_)
            if (r.used && !showing(r, now)) r.used = false;
        return now;
    }

    // The live power and heart rate, every UI pass (a reading is held until
    // the next): averaged over each effort for its result. Invalid = no
    // sensor reporting; that time doesn't count.
    void sample(uint32_t now, uint16_t powerW, bool powerValid, uint8_t hrBpm,
                bool hrValid) {
        if (haveSample_) {
            for (Effort& e : eff_) {
                if (!e.used || e.phase != ACTIVE) continue;
                // Credit the held reading from the later of the last sample
                // and the effort's start; a gap of a minute is not a reading.
                const uint32_t from = (int32_t)(e.startMs - sampleMs_) > 0 ? e.startMs : sampleMs_;
                const uint32_t dt = now - from;
                if ((int32_t)dt <= 0 || dt >= 60000) continue;
                if (heldPowerValid_) { e.energyWms += (double)heldPowerW_ * dt; e.powerMs += dt; }
                if (heldHrValid_) { e.hrBeatMs += (double)heldHr_ * dt; e.hrMs += dt; }
            }
        }
        sampleMs_ = now;
        heldPowerW_ = powerW;
        heldPowerValid_ = powerValid;
        heldHr_ = hrBpm;
        heldHrValid_ = hrValid;
        haveSample_ = true;
    }

    // One GPS fix, stamped with the millis() it arrived.
    void update(uint32_t now, double lat, double lon) {
        now = tick(now);
        updateCourse(lat, lon);
        haveRider_ = true;
        riderLat_ = lat;
        riderLon_ = lon;
        if (!db_ || db_->count == 0) {
            upCount_ = 0;
            return;
        }
        for (Effort& e : eff_) {
            if (!e.used) continue;
            if (e.phase == ACTIVE) trackActive(e, now, lat, lon);
            else trackArmed(e, now, lat, lon);
        }
        scan(now, lat, lon);
    }

    // The primary segment's view (or, with none, just the count).
    void view(uint32_t now, View& v) const { view(now, primary(now), v); }

    // The view of segment `seg` (one from list()); -1 = none.
    void view(uint32_t now, int seg, View& v) const {
        v = View();
        v.count = db_ ? db_->count : 0;
        if (!db_ || seg < 0 || seg >= db_->count) return;
        v.db = db_;
        place(v, seg);
        const Segment& s = db_->segs[seg];
        strncpy(v.name, s.name, sizeof(v.name) - 1);
        v.prSec = s.prSec;
        v.lenM = s.lenM;
        v.gradeD = s.gradeD;
        if (const Effort* e = effortFor(seg, ACTIVE)) {
            v.phase = ACTIVE;
            v.doneM = e->progressM;
            v.elapsedMs = now - e->startMs;
            v.offRoute = e->offSinceMs != 0;
            if (s.prSec > 0 && s.lenM > 0) {
                const float f = e->progressM / s.lenM;
                v.haveDelta = true;
                v.deltaMs = (int32_t)v.elapsedMs - (int32_t)(s.prSec * 1000.0f * f);
            }
            return;
        }
        if (const Result* r = resultFor(seg, now)) {
            v.phase = r->outcome;
            v.elapsedMs = r->ms;
            if (r->outcome == DONE) {
                v.doneM = s.lenM;
                v.prSec = r->prBefore;
                v.newPr = r->newPr;
                v.avgPowerW = r->powerW;
                v.avgHrBpm = r->hr;
                if (r->prBefore > 0) {
                    v.haveDelta = true;
                    v.deltaMs = (int32_t)r->ms - (int32_t)(r->prBefore * 1000);
                }
            } else {
                v.doneM = r->doneM;
            }
            return;
        }
        // Armed or upcoming: where its start is.
        const Effort* armed = effortFor(seg, ARMED);
        v.phase = armed ? ARMED : IDLE;
        for (int i = 0; i < upCount_; ++i)
            if (upSeg_[i] == seg) {
                v.haveNearest = true;
                v.nearestM = upM_[i];
                v.nearestBearingDeg = upBrg_[i];
            }
        if (armed && !v.haveNearest) {
            v.haveNearest = true;
            v.nearestM = armed->startDistM;
            v.nearestBearingDeg = v.riderValid ? bearingDeg(-v.riderX, -v.riderY) : 0;
        }
        if (v.haveNearest) {
            strncpy(v.nearestName, s.name, sizeof(v.nearestName) - 1);
            v.nearestPrSec = s.prSec;
            v.nearestLenM = s.lenM;
            v.nearestGradeD = s.gradeD;
        }
    }

private:
    struct Effort {
        bool used = false;
        int seg = -1;
        Phase phase = IDLE;          // ARMED or ACTIVE
        float startDistM = 0;        // ARMED: from the start now
        float bestStartM = 0;
        uint32_t bestStartMs = 0;
        uint32_t startMs = 0;
        float progressM = 0;
        uint32_t offSinceMs = 0;
        bool finishing = false;
        float bestEndM = 0;
        uint32_t bestEndMs = 0;
        double energyWms = 0, hrBeatMs = 0;
        uint32_t powerMs = 0, hrMs = 0;
    };
    // A finished or abandoned effort, on show for RESULT_MS / NOTE_MS.
    struct Result {
        bool used = false;
        int seg = -1;
        Phase outcome = DONE;
        uint32_t atMs = 0, startMs = 0, ms = 0;
        float doneM = 0;
        uint32_t prBefore = 0;
        bool newPr = false;
        uint16_t powerW = 0xFFFF;
        uint8_t hr = 0;
    };

    SegmentDb* db_ = nullptr;
    Effort eff_[MAX_EFFORTS];
    Result res_[MAX_RESULTS];
    int upSeg_[MAX_UPCOMING];
    float upM_[MAX_UPCOMING], upBrg_[MAX_UPCOMING];
    int upCount_ = 0;
    uint32_t lastNow_ = 0, epoch_ = 0;
    bool haveNow_ = false;
    bool started_ = false, ended_ = false;
    int startedSeg_ = -1;

    // Rider course from displacement between fixes.
    bool haveAnchor_ = false;
    double anchorLat_ = 0, anchorLon_ = 0;
    bool haveCourse_ = false;
    float courseDeg_ = 0;
    // Last fix, for the rider marker on the page's map.
    bool haveRider_ = false;
    double riderLat_ = 0, riderLon_ = 0;
    // The held sensor reading (sample()).
    uint32_t sampleMs_ = 0;
    bool haveSample_ = false;
    uint16_t heldPowerW_ = 0;
    bool heldPowerValid_ = false;
    uint8_t heldHr_ = 0;
    bool heldHrValid_ = false;

    static bool showing(const Result& r, uint32_t now) {
        if (!r.used) return false;
        return now - r.atMs < (r.outcome == DONE ? RESULT_MS : NOTE_MS);
    }
    const Effort* effortFor(int seg, Phase ph) const {
        for (const Effort& e : eff_) if (e.used && e.seg == seg && e.phase == ph) return &e;
        return nullptr;
    }
    const Result* resultFor(int seg, uint32_t now) const {
        const Result* best = nullptr;
        for (const Result& r : res_)
            if (showing(r, now) && r.seg == seg && (!best || r.atMs > best->atMs)) best = &r;
        return best;
    }
    Phase phaseOf(int seg, uint32_t now) const {
        if (seg < 0) return IDLE;
        if (effortFor(seg, ACTIVE)) return ACTIVE;
        if (const Result* r = resultFor(seg, now)) return r->outcome;
        if (effortFor(seg, ARMED)) return ARMED;
        return IDLE;
    }

    void place(View& v, int idx) const {
        v.segIndex = idx;
        v.riderValid = haveRider_;
        if (haveRider_) local(db_->segs[idx], riderLat_, riderLon_, v.riderX, v.riderY);
    }

    void local(const Segment& s, double lat, double lon, float& x, float& y) const {
        x = (float)((lon - s.lon0) * s.mPerDegLon);
        y = (float)((lat - s.lat0) * M_PER_DEG_LAT);
    }

    void updateCourse(double lat, double lon) {
        if (!haveAnchor_) {
            anchorLat_ = lat;
            anchorLon_ = lon;
            haveAnchor_ = true;
            return;
        }
        const float mx = (float)(M_PER_DEG_LON_EQ * cos(lat * M_PI / 180.0));
        const float dx = (float)((lon - anchorLon_) * mx);
        const float dy = (float)((lat - anchorLat_) * M_PER_DEG_LAT);
        if (dx * dx + dy * dy >= COURSE_MIN_MOVE_M * COURSE_MIN_MOVE_M) {
            courseDeg_ = bearingDeg(dx, dy);
            haveCourse_ = true;
            anchorLat_ = lat;
            anchorLon_ = lon;
        }
    }

    // Distance from local (px,py) to segment `s`, over the legs that overlap
    // [fromM, toM] of along-distance; `along` = where the nearest point is.
    float project(const Segment& s, float px, float py, float fromM, float toM,
                  float& along) const {
        const float* X = db_->x + s.first;
        const float* Y = db_->y + s.first;
        const float* C = db_->cum + s.first;
        float best = 1e30f;
        along = 0;
        for (int i = 0; i + 1 < s.nPts; ++i) {
            if (C[i + 1] < fromM) continue;
            if (C[i] > toM) break;
            const float ax = X[i], ay = Y[i];
            const float ex = X[i + 1] - ax, ey = Y[i + 1] - ay;
            const float L2 = ex * ex + ey * ey;
            float t = L2 > 0 ? ((px - ax) * ex + (py - ay) * ey) / L2 : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            const float qx = ax + t * ex - px, qy = ay + t * ey - py;
            const float d2 = qx * qx + qy * qy;
            if (d2 < best) {
                best = d2;
                along = C[i] + t * (C[i + 1] - C[i]);
            }
        }
        return sqrtf(best);
    }

    bool inEffort(int seg) const {
        for (const Effort& e : eff_) if (e.used && e.seg == seg) return true;
        return false;
    }

    // Every segment: the nearest starts (for the page) and whether the rider
    // is at one. Cheap: a box test per segment, then one distance. Starts of
    // segments with an effort already going are skipped.
    void scan(uint32_t now, double lat, double lon) {
        upCount_ = 0;
        const double padLat = NEARBY_M / M_PER_DEG_LAT;
        for (int i = 0; i < db_->count; ++i) {
            if (inEffort(i)) continue;
            const Segment& s = db_->segs[i];
            const double padLon = NEARBY_M / (s.mPerDegLon > 1 ? s.mPerDegLon : 1);
            if (lat < s.minLat - padLat || lat > s.maxLat + padLat ||
                lon < s.minLon - padLon || lon > s.maxLon + padLon)
                continue;
            float x, y;
            local(s, lat, lon, x, y);
            const float d = sqrtf(x * x + y * y);
            // Armed on proximity alone: the rider may still be turning onto
            // the first leg. Setting off along it (heading checked then)
            // is what starts the clock.
            if (d <= ARM_RADIUS_M) {
                for (Effort& e : eff_)
                    if (!e.used) {
                        e = Effort();
                        e.used = true;
                        e.seg = i;
                        e.phase = ARMED;
                        e.startDistM = e.bestStartM = d;
                        e.bestStartMs = now;
                        break;
                    }
                if (inEffort(i)) continue;
            }
            if (d > NEARBY_M) continue;
            // Keep the MAX_UPCOMING nearest, sorted.
            int at = upCount_;
            while (at > 0 && upM_[at - 1] > d) --at;
            if (at >= MAX_UPCOMING) continue;
            const int last = upCount_ < MAX_UPCOMING ? upCount_ : MAX_UPCOMING - 1;
            for (int k = last; k > at; --k) {
                upSeg_[k] = upSeg_[k - 1];
                upM_[k] = upM_[k - 1];
                upBrg_[k] = upBrg_[k - 1];
            }
            upSeg_[at] = i;
            upM_[at] = d;
            upBrg_[at] = bearingDeg(-x, -y);
            if (upCount_ < MAX_UPCOMING) ++upCount_;
        }
    }

    bool headingOk(const Segment& s) const {
        return haveCourse_ && angleDiff(courseDeg_, s.startBearingDeg) <= ARM_HEADING_DEG;
    }

    void trackArmed(Effort& e, uint32_t now, double lat, double lon) {
        const Segment& s = db_->segs[e.seg];
        float x, y;
        local(s, lat, lon, x, y);
        const float d = sqrtf(x * x + y * y);
        e.startDistM = d;
        if (d <= e.bestStartM + 2.0f) {   // ties go to the latest fix
            if (d < e.bestStartM) e.bestStartM = d;
            e.bestStartMs = now;
        }
        float along;
        const float off = project(s, x, y, 0, 3 * ARM_RADIUS_M, along);
        const bool underWay = off <= OFF_ROUTE_M && along >= GO_ALONG_M && headingOk(s);
        if (underWay && (d > ARM_RADIUS_M || along >= 2 * GO_ALONG_M)) {
            e.phase = ACTIVE;
            e.startMs = e.bestStartMs;
            e.progressM = along;
            e.offSinceMs = 0;
            e.finishing = false;
            e.energyWms = e.hrBeatMs = 0;
            e.powerMs = e.hrMs = 0;
            started_ = true;
            startedSeg_ = e.seg;
            return;
        }
        // Left the start circle without setting off along it: not this one.
        if (d > ARM_RADIUS_M) e = Effort();
    }

    void trackActive(Effort& e, uint32_t now, double lat, double lon) {
        const Segment& s = db_->segs[e.seg];
        float x, y;
        local(s, lat, lon, x, y);
        float along;
        const float off = project(s, x, y, e.progressM - WINDOW_BACK_M,
                                  e.progressM + WINDOW_AHEAD_M, along);
        if (off <= OFF_ROUTE_M) {
            e.offSinceMs = 0;
            if (along > e.progressM) e.progressM = along;
        } else {
            if (e.offSinceMs == 0) e.offSinceMs = now;
            if (now - e.offSinceMs >= OFF_ROUTE_MS) {
                endWith(e, ABANDONED, now, now - e.startMs);
                return;
            }
        }
        // The finish: near the end point and most of the way along. Timed at
        // the closest fix (the earliest on a tie, so stopping on the line
        // adds nothing).
        const int last = s.first + s.nPts - 1;
        const float ex = x - db_->x[last], ey = y - db_->y[last];
        const float dEnd = sqrtf(ex * ex + ey * ey);
        if (!e.finishing) {
            if (dEnd <= FINISH_RADIUS_M && e.progressM >= FINISH_MIN_FRACTION * s.lenM) {
                e.finishing = true;
                e.bestEndM = dEnd;
                e.bestEndMs = now;
            }
            return;
        }
        if (dEnd < e.bestEndM) {
            e.bestEndM = dEnd;
            e.bestEndMs = now;
            return;
        }
        // Moving away again: the closest fix was the line.
        if (dEnd > e.bestEndM + 3.0f || dEnd > FINISH_RADIUS_M) finish(e, now);
    }

    void finish(Effort& e, uint32_t now) {
        Segment& s = db_->segs[e.seg];
        const uint32_t ms = e.bestEndMs - e.startMs;
        const uint32_t prBefore = s.prSec;
        const uint32_t sec = (ms + 500) / 1000;
        const bool newPr = s.prSec == 0 || sec < s.prSec;
        // The device's own idea of the PR until the next sync brings
        // Strava's (which only counts once the ride is uploaded).
        if (newPr && sec > 0) s.prSec = sec;
        Result& r = endWith(e, DONE, now, ms);
        r.prBefore = prBefore;
        r.newPr = newPr;
    }

    Result& endWith(Effort& e, Phase outcome, uint32_t now, uint32_t ms) {
        // A free slot, else the one that has been on show longest.
        Result* r = &res_[0];
        for (Result& c : res_) {
            if (!c.used) { r = &c; break; }
            if (c.atMs - r->atMs > 0x80000000u) r = &c;   // older (wrap-safe)
        }
        *r = Result();
        r->used = true;
        r->seg = e.seg;
        r->outcome = outcome;
        r->atMs = now;
        r->startMs = e.startMs;
        r->ms = ms;
        r->doneM = e.progressM;
        r->powerW = e.powerMs > 0 ? (uint16_t)(e.energyWms / e.powerMs + 0.5) : 0xFFFF;
        r->hr = e.hrMs > 0 ? (uint8_t)(e.hrBeatMs / e.hrMs + 0.5) : 0;
        e = Effort();
        ended_ = true;
        return *r;
    }
};

// Encoder, for the host tests and as the reference the apps' encoders are
// checked against. Returns bytes written, 0 when it does not fit or a
// segment breaks the format's limits. `version` 1 drops the elevation.
struct SourceSegment {
    uint64_t id;
    const char* name;
    uint32_t prSec;
    uint32_t distDm;
    int16_t gradeD;
    int nPts;
    const int32_t* latE5;
    const int32_t* lonE5;
    const float* elevM = nullptr;   // nullptr = no elevation
};

inline size_t encodeBlob(const SourceSegment* src, int count, uint8_t* out,
                         size_t cap, int version = BLOB_VERSION) {
    if (count < 0 || count > MAX_SEGMENTS) return 0;
    if (version < BLOB_VERSION_MIN || version > BLOB_VERSION) return 0;
    size_t n = 0;
    auto put = [&](const void* p, size_t k) {
        if (n + k > cap) return false;
        memcpy(out + n, p, k);
        n += k;
        return true;
    };
    auto le = [&](uint64_t v, int bytes) {
        uint8_t b[8];
        for (int i = 0; i < bytes; ++i) b[i] = (uint8_t)(v >> (8 * i));
        return put(b, bytes);
    };
    if (!put("OTSG", 4) || !le(version, 1) || !le(0, 1) || !le(count, 2)) return 0;
    for (int i = 0; i < count; ++i) {
        const SourceSegment& s = src[i];
        size_t nl = strlen(s.name);
        if (nl > NAME_BYTES) nl = NAME_BYTES;
        if (s.nPts < 2 || s.nPts > MAX_POINTS) return 0;
        if (!le(s.id, 8) || !le(s.prSec, 4) || !le(s.distDm, 4) ||
            !le((uint16_t)s.gradeD, 2) || !le(s.nPts, 2))
            return 0;
        if (version >= 2 && !le(s.elevM ? 1 : 0, 1)) return 0;
        if (!le(nl, 1) || !put(s.name, nl)) return 0;
        for (int k = 0; k < s.nPts; ++k) {
            if (!le((uint32_t)s.latE5[k], 4) || !le((uint32_t)s.lonE5[k], 4)) return 0;
            if (version >= 2) {
                long dm = s.elevM ? lroundf((s.elevM[k] + ELEV_OFFSET_M) * 10.0f) : 0;
                dm = dm < 0 ? 0 : dm > 65535 ? 65535 : dm;
                if (!le((uint64_t)dm, 2)) return 0;
            }
        }
    }
    return n;
}

}  // namespace seg
