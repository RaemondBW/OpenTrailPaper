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

// Loaded but no session yet (never started, or stopped): the page's READY
// state, where the left strip goes back to the picker.
inline bool workoutViewReady(const WorkoutView& v) {
    return v.loaded && !v.running && !v.paused;
}

// --- The on-device workout picker --------------------------------------------
// What the WORKOUT page lists when nothing is loaded: one row per file in
// /workouts. Summaries are computed once, when the firmware scans the card
// (workout_service, off the UI task), so drawing the list never touches SD.

constexpr int WORKOUT_SPARK_N = 48;    // intensity samples across the duration
constexpr int WORKOUT_PICK_ROWS = 8;   // rows per page (matches the block list)

struct WorkoutFileInfo {
    char file[48] = "";      // the filename, extension included: the load key
    char title[40] = "";     // the header's DESCRIPTION, else the filename stem
    uint32_t totalSec = 0;
    uint8_t segCount = 0;
    bool ok = false;         // false: didn't parse (listed, but not loadable)
    // Relative intensity, 0..255 against the file's own peak, sampled evenly
    // over the duration: the row's sparkline.
    uint8_t spark[WORKOUT_SPARK_N] = {};
};

// Title for the list: a short DESCRIPTION from the header (the companion
// apps' builders write the workout's name there) or, failing that, the
// filename without its extension. ASCII only, for the panel's fonts.
void workoutTitleFrom(const char* text, const char* file, char* out,
                      size_t cap);

// Fill totalSec / segCount / spark / ok from a parsed workout. `ftpWatts`
// (the one it was parsed with) scales the sparkline; 0 = its own peak.
void workoutSummarize(const Workout& w, int ftpWatts, WorkoutFileInfo& out);

enum WorkoutListState : uint8_t {
    WLIST_SCANNING = 0,   // not read yet / reading now
    WLIST_READY,          // rows are current
    WLIST_NO_CARD,        // no card, or a computer owns it over USB
};

// One page of the picker, copied out of the service's cache for one frame.
struct WorkoutPickPage {
    uint8_t state = WLIST_SCANNING;
    int total = 0;           // files in the whole list
    int first = 0;           // list index of rows[0]
    int count = 0;           // rows filled on this page
    WorkoutFileInfo rows[WORKOUT_PICK_ROWS];
    char loading[48] = "";   // file a tap asked to load, until it lands
    char error[64] = "";     // why the last device-side load failed ("" none)
};
