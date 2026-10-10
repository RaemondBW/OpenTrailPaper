#include "workout_service.h"

#include <Arduino.h>
#include <SD.h>

#include "diag.h"
#include "sd_bus.h"
#include "settings.h"
#include "ride_state.h"
#include "ride_recorder.h"
#include "usb_storage.h"

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

// --- Picker cache ---------------------------------------------------------
// Two PSRAM arrays: the scan fills the back one while the UI keeps reading
// the front, then they swap under g_listMx — a scan never holds the lock
// across SD reads. Allocated on the first scan, so a rider who never opens
// the picker pays nothing.
constexpr int LIST_MAX = 96;
SemaphoreHandle_t g_listMx = nullptr;   // list pointers, pending load, error
SemaphoreHandle_t g_loadMx = nullptr;   // serializes load() (loop vs. srv task)
SemaphoreHandle_t g_wake = nullptr;     // gives serviceFor() work to do
WorkoutFileInfo* g_list = nullptr;      // front: what listPage() reads
WorkoutFileInfo* g_back = nullptr;
int g_listCount = 0;
uint8_t g_listState = WLIST_SCANNING;
volatile bool g_scanReq = false;
char g_pendingLoad[48] = "";
char g_loading[48] = "";                // shown on the picker until it lands
char g_loadError[64] = "";
volatile uint32_t g_version = 0;

void bump() { g_version = g_version + 1; }

struct ListLock {
    ListLock() { if (g_listMx) xSemaphoreTake(g_listMx, portMAX_DELAY); }
    ~ListLock() { if (g_listMx) xSemaphoreGive(g_listMx); }
};

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

static bool loadImpl(const char* name, const char** reason) {
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

bool load(const char* name, const char** reason) {
    // The app's [0x11] runs on the BLE server task and a picker tap on the
    // loop task; both write g_wk, so one at a time.
    if (g_loadMx) xSemaphoreTake(g_loadMx, portMAX_DELAY);
    bool ok = loadImpl(name, reason);
    if (g_loadMx) xSemaphoreGive(g_loadMx);
    bump();
    return ok;
}

// --- Picker ------------------------------------------------------------------

void begin() {
    if (g_listMx) return;
    g_listMx = xSemaphoreCreateMutex();
    g_loadMx = xSemaphoreCreateMutex();
    g_wake = xSemaphoreCreateBinary();
}

void requestListRefresh() {
    {
        ListLock l;
        g_loadError[0] = 0;   // a failure from an earlier visit is old news
    }
    g_scanReq = true;
    if (g_wake) xSemaphoreGive(g_wake);
}

void requestLoad(const char* file) {
    if (!file || !file[0]) return;
    {
        ListLock l;
        snprintf(g_pendingLoad, sizeof(g_pendingLoad), "%s", file);
        snprintf(g_loading, sizeof(g_loading), "%s", file);
        g_loadError[0] = 0;
    }
    bump();
    if (g_wake) xSemaphoreGive(g_wake);
}

void listPage(int page, WorkoutPickPage& out) {
    ListLock l;
    out.state = g_listState;
    out.total = g_listCount;
    if (page < 0) page = 0;
    out.first = page * WORKOUT_PICK_ROWS;
    out.count = 0;
    for (int i = out.first; i < g_listCount && out.count < WORKOUT_PICK_ROWS;
         ++i)
        out.rows[out.count++] = g_list[i];
    snprintf(out.loading, sizeof(out.loading), "%s", g_loading);
    snprintf(out.error, sizeof(out.error), "%s", g_loadError);
}

uint32_t version() { return g_version; }

namespace {

bool cardAvailable() {
    return ride_recorder::sdMounted() && !usb_storage::hostActive();
}

bool isWorkoutFile(const char* base) {
    if (base[0] == '.') return false;   // macOS AppleDouble noise
    const char* dot = strrchr(base, '.');
    return dot && (!strcasecmp(dot, ".erg") || !strcasecmp(dot, ".mrc"));
}

int byTitle(const void* a, const void* b) {
    return strcasecmp(((const WorkoutFileInfo*)a)->title,
                      ((const WorkoutFileInfo*)b)->title);
}

// Runs on the loop task. Names first (one short SD hold for the directory),
// then each file read under its own hold — a recorder flush waits at most one
// small file, never the whole scan.
void scan() {
    if (!cardAvailable()) {
        ListLock l;
        g_listState = WLIST_NO_CARD;
        bump();
        return;
    }
    if (!g_list) {
        size_t sz = sizeof(WorkoutFileInfo) * LIST_MAX;
        g_list = (WorkoutFileInfo*)heap_caps_calloc(1, sz, MALLOC_CAP_SPIRAM);
        g_back = (WorkoutFileInfo*)heap_caps_calloc(1, sz, MALLOC_CAP_SPIRAM);
        if (!g_list || !g_back) {
            heap_caps_free(g_list);
            heap_caps_free(g_back);
            g_list = g_back = nullptr;
            diag::log("workout: no PSRAM for the picker list");
            return;
        }
    }
    // The parse target and file text: PSRAM, not this task's stack.
    static Workout* scratch = nullptr;
    if (!scratch)
        scratch = (Workout*)heap_caps_malloc(sizeof(Workout), MALLOC_CAP_SPIRAM);
    char* buf = (char*)heap_caps_malloc(MAX_FILE + 1, MALLOC_CAP_SPIRAM);
    if (!scratch || !buf) {
        heap_caps_free(buf);
        return;
    }

    int n = 0;
    sdLock();
    File dir = SD.open(DIR);
    if (dir) {
        for (File f = dir.openNextFile(); f && n < LIST_MAX;
             f = dir.openNextFile()) {
            if (f.isDirectory()) continue;
            const char* base = strrchr(f.name(), '/');
            base = base ? base + 1 : f.name();
            if (!isWorkoutFile(base) || strlen(base) >= sizeof(g_back[0].file))
                continue;
            g_back[n] = WorkoutFileInfo{};
            snprintf(g_back[n].file, sizeof(g_back[n].file), "%s", base);
            ++n;
        }
        dir.close();
    }
    sdUnlock();

    // A percent (.mrc) file needs an FTP to become watts. Duration and shape
    // don't depend on which, so an unset FTP still lists every file; the
    // sparkline is then scaled to each file's own peak instead of to FTP.
    const int ftp = settings::ftpWatts() > 0 ? settings::ftpWatts() : 200;
    for (int i = 0; i < n; ++i) {
        WorkoutFileInfo& e = g_back[i];
        char path[96];
        snprintf(path, sizeof(path), "%s/%s", DIR, e.file);
        size_t got = 0;
        sdLock();
        File f = SD.open(path, FILE_READ);
        if (f) {
            if (f.size() <= MAX_FILE) got = f.read((uint8_t*)buf, f.size());
            f.close();
        }
        sdUnlock();
        buf[got] = 0;
        workoutTitleFrom(buf, e.file, e.title, sizeof(e.title));
        if (got && workoutParse(buf, ftp, *scratch))
            workoutSummarize(*scratch, settings::ftpWatts() > 0 ? ftp : 0, e);
        vTaskDelay(1);   // let the recorder in between files
    }
    heap_caps_free(buf);
    qsort(g_back, n, sizeof(WorkoutFileInfo), byTitle);

    {
        ListLock l;
        WorkoutFileInfo* t = g_list;
        g_list = g_back;
        g_back = t;
        g_listCount = n;
        g_listState = WLIST_READY;
    }
    bump();
    diag::log("workout: picker list has %d file(s)", n);
}

}  // namespace

void serviceFor(uint32_t ms) {
    const uint32_t t0 = millis();
    for (;;) {
        // The card coming back (remount after a drop, a USB host letting
        // go) rescans; going away shows "no card" until it returns. Only
        // once a list exists — nobody opened the picker, nobody waits.
        static bool wasAvail = false;
        const bool avail = cardAvailable();
        if (avail != wasAvail) {
            wasAvail = avail;
            if (g_list || g_listState != WLIST_SCANNING) g_scanReq = true;
        }
        if (g_scanReq) {
            g_scanReq = false;
            scan();
        }
        char name[48] = "";
        {
            ListLock l;
            if (g_pendingLoad[0]) {
                snprintf(name, sizeof(name), "%s", g_pendingLoad);
                g_pendingLoad[0] = 0;
            }
        }
        if (name[0]) {
            const char* reason = "";
            bool ok = load(name, &reason);
            {
                ListLock l;
                // A newer tap may have queued meanwhile; leave its marker.
                if (!strcmp(g_loading, name)) g_loading[0] = 0;
                if (!ok)
                    snprintf(g_loadError, sizeof(g_loadError), "%s: %s", name,
                             reason);
            }
            diag::log("workout: picker load %s -> %s", name, ok ? "ok" : reason);
            bump();
        }
        const uint32_t spent = millis() - t0;
        if (spent >= ms) return;
        if (g_wake) xSemaphoreTake(g_wake, pdMS_TO_TICKS(ms - spent));
        else vTaskDelay(pdMS_TO_TICKS(ms - spent));
    }
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
    bump();
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
