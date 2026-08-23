#include "workout.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

// --- Parsing -----------------------------------------------------------------

// Case-insensitive "does this line contain `word`" over one line only.
static bool lineHas(const char* line, const char* lineEnd, const char* word) {
    size_t wl = strlen(word);
    for (const char* p = line; p + wl <= lineEnd; ++p)
        if (!strncasecmp(p, word, wl)) return true;
    return false;
}

bool workoutParse(const char* text, int ftpWatts, Workout& out) {
    out = Workout{};
    if (!text) return false;

    // One pass, line by line. The [COURSE DATA] section carries the points;
    // before it, the header may declare "MINUTES PERCENT" (an .mrc) — default
    // is WATTS (an .erg). An FTP= line in the header wins over the setting,
    // since a file exported for a specific rider knows better.
    bool inData = false;
    bool percent = false;
    double pt[WORKOUT_MAX_SEGS + 1][2];
    int npt = 0;

    const char* p = text;
    while (*p) {
        const char* lineEnd = p;
        while (*lineEnd && *lineEnd != '\n') ++lineEnd;

        if (!inData) {
            if (lineHas(p, lineEnd, "[COURSE DATA]")) {
                inData = true;
            } else if (lineHas(p, lineEnd, "PERCENT")) {
                percent = true;
            } else if (lineHas(p, lineEnd, "FTP")) {
                const char* eq = p;
                while (eq < lineEnd && *eq != '=') ++eq;
                if (eq < lineEnd) {
                    int v = atoi(eq + 1);
                    if (v >= 50 && v <= 600) ftpWatts = v;
                }
            }
        } else if (lineHas(p, lineEnd, "[END COURSE DATA]")) {
            break;
        } else {
            // "minutes<tab/space>value" — anything that doesn't start with a
            // digit (blank lines, stray headers) is skipped, not fatal.
            const char* q = p;
            while (q < lineEnd && (*q == ' ' || *q == '\t' || *q == '\r')) ++q;
            if (q < lineEnd && ((*q >= '0' && *q <= '9') || *q == '.')) {
                char* after = nullptr;
                double mins = strtod(q, &after);
                if (after && after > q) {
                    double val = strtod(after, &after);
                    if (val > 0 && npt <= WORKOUT_MAX_SEGS) {
                        pt[npt][0] = mins;
                        pt[npt][1] = val;
                        ++npt;
                    }
                }
            }
        }
        p = *lineEnd ? lineEnd + 1 : lineEnd;
    }

    if (npt < 2) return false;
    if (percent && ftpWatts <= 0) return false;

    // Points -> segments. A pair whose time matches its predecessor is a step
    // edge, not a segment; a pair that advances time closes one.
    for (int i = 1; i < npt && out.count < WORKOUT_MAX_SEGS; ++i) {
        double t0 = pt[i - 1][0] * 60.0, t1 = pt[i][0] * 60.0;
        if (t1 <= t0 + 0.001) continue;   // step edge (or out-of-order noise)
        double w0 = pt[i - 1][1], w1 = pt[i][1];
        if (percent) { w0 = w0 * ftpWatts / 100.0; w1 = w1 * ftpWatts / 100.0; }
        WorkoutSeg& s = out.segs[out.count++];
        s.startSec = (uint32_t)(t0 + 0.5);
        s.endSec = (uint32_t)(t1 + 0.5);
        s.startW = (uint16_t)(w0 + 0.5);
        s.endW = (uint16_t)(w1 + 0.5);
    }
    if (out.count == 0) return false;
    out.totalSec = out.segs[out.count - 1].endSec;
    return true;
}

// --- Time math ---------------------------------------------------------------

uint16_t workoutTargetAt(const Workout& w, uint32_t sec, int* segIdx) {
    if (w.count == 0) {
        if (segIdx) *segIdx = 0;
        return 0;
    }
    for (int i = 0; i < w.count; ++i) {
        const WorkoutSeg& s = w.segs[i];
        if (sec < s.endSec || i == w.count - 1) {
            if (segIdx) *segIdx = i;
            if (sec >= s.endSec) return s.endW;   // past the end: hold last
            uint32_t len = s.endSec - s.startSec;
            if (len == 0 || s.startW == s.endW) return s.startW;
            uint32_t in = sec > s.startSec ? sec - s.startSec : 0;
            return (uint16_t)(s.startW +
                              (int32_t)(s.endW - s.startW) * (int32_t)in /
                                  (int32_t)len);
        }
    }
    if (segIdx) *segIdx = w.count - 1;
    return w.segs[w.count - 1].endW;
}

int workoutZone(uint16_t watts, uint16_t ftp) {
    if (ftp == 0) return 0;
    int pct = (int)watts * 100 / ftp;
    if (pct <= 55) return 1;
    if (pct <= 75) return 2;
    if (pct <= 90) return 3;
    if (pct <= 105) return 4;
    if (pct <= 120) return 5;
    if (pct <= 150) return 6;
    return 7;
}

const char* workoutZoneName(int zone) {
    // Impact_T is subsetted to caps + digits, so these stay uppercase.
    switch (zone) {
        case 1: return "RECOVERY";
        case 2: return "ENDURANCE";
        case 3: return "TEMPO";
        case 4: return "THRESHOLD";
        case 5: return "VO2 MAX";
        case 6: return "ANAEROBIC";
        case 7: return "NEUROMUSC";
        default: return "";
    }
}

void workoutBuildView(const Workout& w, uint32_t elapsedSec, bool running,
                      uint16_t ftpW, WorkoutView& v) {
    v = WorkoutView{};
    if (w.count == 0) return;
    v.loaded = true;
    v.running = running;
    v.wk = &w;
    memcpy(v.name, w.name, sizeof(v.name));
    v.name[sizeof(v.name) - 1] = 0;
    v.totalSec = w.totalSec;
    v.segCount = w.count;
    v.ftpW = ftpW;
    v.elapsedSec = elapsedSec;
    v.done = elapsedSec >= w.totalSec;
    if (v.done) v.elapsedSec = w.totalSec;
    v.targetW = workoutTargetAt(w, v.elapsedSec, &v.segIdx);
    const WorkoutSeg& s = w.segs[v.segIdx];
    v.segRemainSec = s.endSec > v.elapsedSec ? s.endSec - v.elapsedSec : 0;
    if (v.segIdx + 1 < w.count) v.nextW = w.segs[v.segIdx + 1].startW;
}
