#pragma once

#include <stdint.h>
#include <stddef.h>

// Structured workout: parse an ERG/MRC file and answer "what power should the
// rider hold right now" for the workout page.
//
// ERG/MRC is the format every training platform can produce — TrainingPeaks,
// TrainerRoad and Zwift all export or convert to it — and it is plain text:
// a [COURSE DATA] section of "minutes value" pairs, WATTS in an .erg and
// PERCENT (of FTP) in an .mrc. Consecutive pairs with advancing time form a
// segment; a pair repeating the previous minute is a step edge. Percent files
// are scaled by the rider's FTP at load, so the rest of the device only ever
// sees watts.
//
// HOST-SAFE. Like dash_layout.*, this pair is compiled by the preview tool
// (tools/preview/render_preview.sh) alongside ui_render.cpp, so nothing here
// may touch Arduino, SD or NVS. Loading the FILE lives in workout_service.*
// on the firmware side; this half only parses text and does the time math.

// 64 is comfortably past real workouts: a 2x20 threshold session is ~10
// segments, and even a microburst set (30/30s for half an hour) fits. Fixed
// POD so the loaded workout can live in a static with no allocation.
constexpr int WORKOUT_MAX_SEGS = 64;

struct WorkoutSeg {
    uint32_t startSec = 0;
    uint32_t endSec = 0;
    uint16_t startW = 0;   // target at the segment's start
    uint16_t endW = 0;     // at its end — different means a ramp
};

struct Workout {
    char name[40] = "";          // from the filename; the page's title
    WorkoutSeg segs[WORKOUT_MAX_SEGS];
    int count = 0;
    uint32_t totalSec = 0;
};

// Parse ERG/MRC text. `ftpWatts` scales PERCENT files (detected from the
// header's "MINUTES PERCENT" column line, falling back to WATTS). Returns
// false when fewer than two course points parse — the current workout is the
// caller's to keep or drop. `name` is NOT set here; the caller knows the
// filename.
bool workoutParse(const char* text, int ftpWatts, Workout& out);

// Target watts at `sec` into the workout, linearly interpolated through ramps
// and clamped to the last segment's end. `segIdx` (optional) receives which
// segment `sec` landed in.
uint16_t workoutTargetAt(const Workout& w, uint32_t sec, int* segIdx);

// Coggan zone for a wattage at a given FTP: 1..7, or 0 when ftp is unset.
int workoutZone(uint16_t watts, uint16_t ftp);
const char* workoutZoneName(int zone);   // "RECOVERY".."NEUROMUSC", "" for 0

// Everything the workout page needs for one frame, derived once per second by
// workoutBuildView so the renderer stays pure drawing.
struct WorkoutView {
    bool loaded = false;
    bool running = false;
    bool paused = false;         // a session exists but the clock is held
    bool done = false;           // elapsed ran past the final segment
    char name[40] = "";
    uint32_t elapsedSec = 0;
    uint32_t totalSec = 0;
    int segIdx = 0;
    int segCount = 0;
    uint32_t segRemainSec = 0;   // countdown inside the current segment
    uint16_t targetW = 0;
    uint16_t nextW = 0;          // next segment's opening target (0 = none)
    uint16_t ftpW = 0;
    const Workout* wk = nullptr; // for the profile strip; never null if loaded
};

void workoutBuildView(const Workout& w, uint32_t elapsedSec, bool running,
                      uint16_t ftpW, WorkoutView& v);
