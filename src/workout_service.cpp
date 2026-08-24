#include "workout_service.h"

#include <Arduino.h>
#include <SD.h>

#include "diag.h"
#include "sd_bus.h"
#include "settings.h"
#include "ride_state.h"

namespace workout_service {
namespace {

constexpr const char* DIR = "/workouts";
// The largest ERG in the wild is a few KB; 16 KB of headroom costs nothing
// against PSRAM and never truncates a real file.
constexpr size_t MAX_FILE = 16 * 1024;

Workout g_wk;
bool g_loaded = false;
bool g_started = false;   // a session exists (running or paused)
bool g_running = false;
uint32_t g_baseSec = 0;   // elapsed accumulated at the last pause/seek
uint32_t g_startMs = 0;   // wall anchor of the running stretch

uint32_t elapsedSec() {
    uint32_t e = g_baseSec;
    if (g_running) e += (millis() - g_startMs) / 1000;
    return e;
}

// Pause-each-block boundary detector state (see tick()). An explicit seek
// disarms it: skipping INTO a block is a deliberate start, not a boundary
// the clock drifted across — it must not immediately pause.
bool g_boundaryArmed = false;
int g_boundaryIdx = -1;

// Move the clock without changing whether it runs — every jump (skip, back,
// tap on the profile) is this.
void seekSec(uint32_t sec) {
    if (sec > g_wk.totalSec) sec = g_wk.totalSec;
    g_baseSec = sec;
    g_startMs = millis();
    g_boundaryArmed = false;
}

}  // namespace

int list(char* out, size_t cap) {
    if (out && cap) out[0] = 0;
    size_t n = 0;
    int count = 0;
    sdLock();
    File dir = SD.open(DIR);
    if (dir) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            if (f.isDirectory()) continue;
            const char* base = strrchr(f.name(), '/');
            base = base ? base + 1 : f.name();
            ++count;
            if (out && n + strlen(base) + 2 < cap)
                n += snprintf(out + n, cap - n, "%s\n", base);
        }
        dir.close();
    }
    sdUnlock();
    return count;
}

bool load(const char* name, const char** reason) {
    static const char* kNoCard = "SD not available";
    static const char* kNoFile = "file not found";
    static const char* kTooBig = "file too large";
    static const char* kBadParse = "no course data parsed";
    static const char* kOk = "ok";
    if (reason) *reason = kOk;

    stop();
    g_loaded = false;

    char path[96];
    snprintf(path, sizeof(path), "%s/%s", DIR, name);

    char* buf = (char*)heap_caps_malloc(MAX_FILE + 1, MALLOC_CAP_SPIRAM);
    if (!buf) { if (reason) *reason = kNoCard; return false; }

    sdLock();
    File f = SD.open(path, FILE_READ);
    if (!f) {
        sdUnlock();
        heap_caps_free(buf);
        if (reason) *reason = kNoFile;
        return false;
    }
    size_t sz = f.size();
    if (sz > MAX_FILE) {
        f.close();
        sdUnlock();
        heap_caps_free(buf);
        if (reason) *reason = kTooBig;
        return false;
    }
    size_t got = f.read((uint8_t*)buf, sz);
    f.close();
    sdUnlock();
    buf[got] = 0;

    bool ok = workoutParse(buf, settings::ftpWatts(), g_wk);
    heap_caps_free(buf);
    if (!ok) {
        if (reason) *reason = kBadParse;
        diag::log("workout: %s failed to parse", name);
        return false;
    }
    snprintf(g_wk.name, sizeof(g_wk.name), "%s", name);
    // The filename is the title; drop the extension so the page doesn't
    // read "SWEETSPOT.ERG". Uppercase for the Impact faces' subset.
    char* dot = strrchr(g_wk.name, '.');
    if (dot) *dot = 0;
    for (char* c = g_wk.name; *c; ++c)
        if (*c >= 'a' && *c <= 'z') *c -= 32;
    g_loaded = true;
    diag::log("workout: loaded %s — %d segments, %lu s total", g_wk.name,
              g_wk.count, (unsigned long)g_wk.totalSec);
    return true;
}

void start() {
    if (!g_loaded) return;
    g_started = true;
    g_running = true;
    g_baseSec = 0;
    g_startMs = millis();
    diag::log("workout: started %s", g_wk.name);
}

void stop() {
    if (!g_started) return;
    g_started = false;
    g_running = false;
    g_baseSec = 0;
    diag::log("workout: stopped");
}

void pause() {
    if (!g_running) return;
    g_baseSec = elapsedSec();
    g_running = false;
    diag::log("workout: paused at %lu s", (unsigned long)g_baseSec);
}

void resume() {
    if (!g_started || g_running) return;
    g_startMs = millis();
    g_running = true;
    diag::log("workout: resumed at %lu s", (unsigned long)g_baseSec);
}

void unload() {
    // The app's Stop: not just "clock off" but "put the workout away" — the
    // device page returns to its pick-a-workout state.
    stop();
    g_loaded = false;
    diag::log("workout: unloaded");
}

void toggle() {
    if (!g_loaded) return;
    if (!g_started) start();
    else if (g_running) pause();
    else resume();
}

void skip() {
    if (!g_started || g_wk.count == 0) return;
    int idx = 0;
    workoutTargetAt(g_wk, elapsedSec(), &idx);
    seekSec(g_wk.segs[idx].endSec);
    diag::log("workout: skip -> interval %d/%d",
              idx + 2 > g_wk.count ? g_wk.count : idx + 2, g_wk.count);
}

void prevInterval() {
    if (!g_started || g_wk.count == 0) return;
    int idx = 0;
    workoutTargetAt(g_wk, elapsedSec(), &idx);
    uint32_t segStart = g_wk.segs[idx].startSec;
    // Deep in an interval, back means "this one again"; right at its start it
    // means the one before — the same rule as a track's back button.
    if (elapsedSec() < segStart + 3 && idx > 0)
        segStart = g_wk.segs[idx - 1].startSec;
    seekSec(segStart);
    diag::log("workout: back -> %lu s", (unsigned long)segStart);
}

bool jumpToFraction(float frac) {
    if (!g_loaded || g_wk.count == 0) return false;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int idx = 0;
    workoutTargetAt(g_wk, (uint32_t)(frac * g_wk.totalSec), &idx);
    if (!g_started) { g_started = true; g_running = true; }
    seekSec(g_wk.segs[idx].startSec);
    diag::log("workout: jump -> interval %d/%d", idx + 1, g_wk.count);
    return true;
}

void jumpToSeg(int idx) {
    if (!g_loaded || g_wk.count == 0) return;
    if (idx < 0) idx = 0;
    if (idx >= g_wk.count) idx = g_wk.count - 1;
    g_started = true;
    g_running = true;   // tapping a block means "ride it", not "cue it up"
    seekSec(g_wk.segs[idx].startSec);
    diag::log("workout: start block %d/%d", idx + 1, g_wk.count);
}

bool loaded() { return g_loaded; }
bool running() { return g_running; }

void motionTick();   // defined below; runs every tick regardless of settings

void tick() {
    motionTick();
    // "Pause after every block": catch the clock crossing a boundary and hold
    // it AT the boundary, so resume starts the next block from its first
    // second. Called at 1 Hz from loop(). The detector disarms whenever the
    // clock isn't running AND on every explicit seek (see seekSec) — a skip
    // or a tapped block is a deliberate start of that block, and holding it
    // instantly would turn "next" into "next, but frozen".
    if (!g_running || !settings::workoutPauseEachBlock() || g_wk.count == 0) {
        g_boundaryArmed = false;
        return;
    }
    int idx = 0;
    workoutTargetAt(g_wk, elapsedSec(), &idx);
    if (!g_boundaryArmed) {
        g_boundaryArmed = true;
        g_boundaryIdx = idx;
        return;
    }
    if (idx != g_boundaryIdx) {
        g_baseSec = g_wk.segs[idx].startSec;   // exactly the boundary
        g_running = false;
        g_boundaryArmed = false;
        diag::log("workout: holding at block %d/%d (pause-each-block)",
                  idx + 1, g_wk.count);
    }
}

// Stillness auto-pause: a rider who stops without touching anything should
// not watch their intervals march on. No power AND no movement for 5 s
// pauses the clock; power or movement returning resumes it at once — the
// ERG-trainer convention. Only pauses this code set are auto-resumed:
// an explicit pause, and the pause-each-block boundary hold, stay held.
void motionTick() {
    static uint8_t stillSec = 0;
    static bool autoPaused = false;
    if (!g_started) { stillSec = 0; autoPaused = false; return; }

    RideState st = g_state.snapshot();
    const bool hasPower = st.power3sW != 0xFFFF && st.power3sW > 0;
    const bool moving = (st.gpsFix && st.speedKmh > 1.0f) || st.deviceMoving;

    if (g_running) {
        if (!hasPower && !moving) {
            if (++stillSec >= 5) {
                stillSec = 0;
                pause();
                autoPaused = true;
                diag::log("workout: auto-paused (no power, no movement)");
            }
        } else {
            stillSec = 0;
        }
    } else if (autoPaused && (hasPower || moving)) {
        autoPaused = false;
        resume();
        diag::log("workout: auto-resumed (%s)", hasPower ? "power" : "movement");
    }
}

void view(WorkoutView& v) {
    if (!g_loaded) {
        v = WorkoutView{};
        // The FTP is device state, not workout state — the app's builder
        // needs it before anything is loaded.
        v.ftpW = (uint16_t)settings::ftpWatts();
        return;
    }
    workoutBuildView(g_wk, elapsedSec(), g_running,
                     (uint16_t)settings::ftpWatts(), v);
    v.paused = g_started && !g_running;
}

}  // namespace workout_service
