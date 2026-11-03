// A/B bench for the hybrid update: the same scenes through the shipping driver
// path and through hybrid scanout, on the serial console.
//
// Does not touch the shipping firmware, and is a separate env for the same reason
// tools/epdpainter_test is. What it cannot measure is the point of the exercise:
// ghosting is judged by eye, on the frame each run leaves on the glass.
//   pio run -e epd-hybrid-bench -t upload
//
// `help` on the console lists the commands.
#define EPD_PAINTER_PRESET_LILYGO_T5_S3_GPS

#include <Arduino.h>
#include <esp32-hal-tinyusb.h>
#include <esp_heap_caps.h>
#include <math.h>

#include "EPD_Painter_presets.h"
#include "EPD_Painter.h"
#include "epd_hybrid.h"
#include "test_clip.h"

using hybrid::kHeight;
using hybrid::kPlaneBytes;
using hybrid::kRowBytes;
using hybrid::kWidth;

namespace {

constexpr uint8_t BLACK = 3, WHITE = 0;

EPD_Painter painter(EPD_LILYGO_T5_S3_GPS_PRESET, /*portrait=*/false);
EpdHybrid engine(painter);
uint8_t* frame = nullptr;
int g_resetReason = 0;

inline void pixel(int x, int y, uint8_t level) {
    uint8_t& b = frame[(size_t)y * kRowBytes + (x >> 2)];
    const int shift = 6 - 2 * (x & 3);
    b = (uint8_t)((b & ~(3 << shift)) | (level << shift));
}

void rect(int x0, int y0, int w, int h, uint8_t level) {
    for (int y = y0; y < y0 + h; ++y) {
        if (y < 0 || y >= kHeight) continue;
        for (int x = x0; x < x0 + w; ++x) {
            if (x >= 0 && x < kWidth) pixel(x, y, level);
        }
    }
}

void blank() { memset(frame, 0, kPlaneBytes); }

// --- scenes ------------------------------------------------------------------
// Each draws frame `i` of a run. Between them they cover what actually ghosts on
// the device: the digits that change every second, the map pan that left trails,
// solid black landing on thin lines, and a page flip.

void digit(int x, int y, int value) {
    static const uint8_t kSegs[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    constexpr int W = 170, H = 150, T = 34;
    const uint8_t s = kSegs[value % 10];
    if (s & 0x01) rect(x, y, W, T, BLACK);
    if (s & 0x02) rect(x + W - T, y, T, H, BLACK);
    if (s & 0x04) rect(x + W - T, y + H, T, H, BLACK);
    if (s & 0x08) rect(x, y + 2 * H - T, W, T, BLACK);
    if (s & 0x10) rect(x, y + H, T, H, BLACK);
    if (s & 0x20) rect(x, y, T, H, BLACK);
    if (s & 0x40) rect(x, y + H - T / 2, W, T, BLACK);
}

void sceneDigits(int i) {
    blank();
    rect(0, 0, kWidth, 40, BLACK);
    digit(140, 110, i / 100);
    digit(380, 110, i / 10);
    digit(620, 110, i);
}

void scenePan(int i) {
    blank();
    const int shift = i * 14;
    for (int y = 0; y < kHeight; ++y) {
        const int coast = 420 + (int)(170 * sinf((y + shift) / 90.0f));
        for (int x = 0; x < coast; ++x) {
            if (!((x & 1) && (y & 1))) pixel(x, y, BLACK);   // the map's 75% water
        }
    }
    for (int r = 0; r < 4; ++r) {
        const int y = ((r * 170 + shift) % (kHeight + 60)) - 30;
        rect(0, y, kWidth, 5, WHITE);
        rect(0, y + 1, kWidth, 3, BLACK);
    }
}

void sceneTrail(int i) {
    blank();
    switch (i % 3) {
        case 0:
            for (int k = 0; k < 14; ++k) rect(120 + k * 50, 70, 3, 400, BLACK);
            break;
        case 1:
            rect(100, 60, 760, 420, BLACK);
            break;
        default:
            break;
    }
}

void sceneFlip(int i) {
    blank();
    if (i & 1) {
        for (int y = 30; y < kHeight - 30; y += 44) rect(50, y, 860, 24, BLACK);
    } else {
        for (int x = 40; x < kWidth - 40; x += 120) rect(x, 40, 70, 460, BLACK);
    }
}

struct Scene {
    const char* name;
    void (*draw)(int);
    int frames;
    int hz;
};
const Scene kScenes[] = {
    {"digits", sceneDigits, 60, 2},
    {"pan", scenePan, 48, 8},
    {"trail", sceneTrail, 19, 2},
    {"flip", sceneFlip, 21, 3},
};

// --- runs --------------------------------------------------------------------

// `long` is the existing driver with NORMAL's 13 passes and no gap between them: a
// longer drive than FAST for a config change. Levels 1 and 2 would want retuning at
// that gap, but this clip is black and white only.
//
// `paced` and `strict` differ only in who does the scanning: FAST's 7 passes at a
// constant 10 ms in the driver, and the same in this bench's scan loop. Both draw
// every frame, which `batch` does not.
//
// `topup` is `strict` plus a top-up for the pixels beside the ones that changed.
enum class Mode { Shipping, Fast, Hybrid, HybridNoSettle, Long, Batch, Paced, Strict, TopUp };
const char* const kModeNames[] = {"shipping", "fast",  "hybrid", "nosettle", "long",
                                  "batch",    "paced", "strict", "topup"};
constexpr int kModes = 9;
int g_topUpPasses = 3;
int g_strictPasses = 7;
int g_pacedPassUs = 10000;
int g_pacedRowUs = 0;
int g_eraseK = 2;
int g_longGapUs = 0;
int g_maxFrames = 0;        // 0 = each scene's full length
uint32_t g_holdMs = 2500;   // how long a card stays up after a clip section
bool g_stop = false;

// `x` on the console ends the run at the next frame. Nothing else is read during a
// run, so a stray byte costs a character of the next command at worst.
bool stopRequested() {
    while (Serial.available()) {
        if (Serial.read() == 'x') g_stop = true;
    }
    return g_stop;
}

void pause(uint32_t ms) {
    const uint32_t end = millis() + ms;
    while ((int32_t)(end - millis()) > 0 && !stopRequested()) delay(10);
}
int g_normalGapUs = 0;

bool usesEngine(Mode m) {
    return m == Mode::Hybrid || m == Mode::HybridNoSettle || m == Mode::Batch ||
           m == Mode::Strict || m == Mode::TopUp;
}

bool oneAtATime(Mode m) { return m == Mode::Strict || m == Mode::TopUp; }

bool settles(Mode m) { return m == Mode::Hybrid; }

void configure(Mode mode) {
    const bool fast = mode == Mode::Fast || mode == Mode::Paced;
    painter.setQuality(fast ? EPD_Painter::Quality::QUALITY_FAST
                            : EPD_Painter::Quality::QUALITY_NORMAL);
    painter._config.pass_gap_us_normal = mode == Mode::Long ? g_longGapUs : g_normalGapUs;
    painter._config.pass_us_fast = mode == Mode::Paced ? g_pacedPassUs : 0;
    painter._config.row_extra_us_fast = mode == Mode::Paced ? g_pacedRowUs : 0;
    engine.batch = mode == Mode::Batch || oneAtATime(mode);
    engine.params().topUp = mode == Mode::TopUp ? g_topUpPasses : 0;
    engine.params().eraseK = (uint8_t)g_eraseK;

    // Whatever `set fast` chose comes back once strict is finished.
    static uint8_t chosenPasses = 0;
    hybrid::Params& p = engine.params();
    if (oneAtATime(mode)) {
        if (!chosenPasses) chosenPasses = p.fastPasses;
        p.fastPasses = (uint8_t)g_strictPasses;
    } else if (chosenPasses) {
        p.fastPasses = chosenPasses;
        chosenPasses = 0;
    }
}

void hardClear() {
    painter.clear();
    painter.clear();
    while (!painter.paintIdle()) vTaskDelay(1);
}

void run(const Scene& s, Mode mode, int hz) {
    const bool hybrid = usesEngine(mode);
    const int frames = g_maxFrames && g_maxFrames < s.frames ? g_maxFrames : s.frames;
    Serial.printf("[bench] %s / %s: %d frames at %d Hz\n", s.name,
                  kModeNames[(int)mode], frames, hz);

    configure(mode);
    hardClear();

    const uint32_t savedSettle = engine.params().settleAfterMs;
    if (usesEngine(mode) && !settles(mode)) engine.params().settleAfterMs = 0;
    if (hybrid) engine.engage();

    const uint32_t paintsBefore = painter.paintsCompleted();
    const uint32_t periodMs = 1000 / hz;
    const uint32_t start = millis();
    uint32_t next = start;
    uint32_t drawMax = 0, late = 0;
    for (int i = 0; i < frames && !stopRequested(); ++i) {
        const uint32_t t0 = millis();
        s.draw(i);
        const uint32_t drawn = millis();
        if (drawn - t0 > drawMax) drawMax = drawn - t0;
        if (hybrid) engine.post(frame);
        else painter.paintPacked(frame);
        next += periodMs;
        const int32_t left = (int32_t)(next - millis());
        if (left > 0) pause(left);
        else late++;
    }
    const uint32_t pushed = millis();

    if (hybrid) {
        engine.disengage();
        engine.printStats();
    } else {
        while (!painter.paintIdle()) vTaskDelay(1);
        Serial.printf("[bench] %u drive cycles\n",
                      (unsigned)(painter.paintsCompleted() - paintsBefore));
    }
    engine.params().settleAfterMs = savedSettle;

    Serial.printf("[bench] took %lu ms, wanted %lu; %lu frames late; slowest draw %lu ms; "
                  "%lu ms more to finish\n",
                  (unsigned long)(pushed - start), (unsigned long)(frames * periodMs),
                  (unsigned long)late, (unsigned long)drawMax, (unsigned long)(millis() - pushed));
    Serial.println("[bench] last frame left on the glass: look for ghosts now");
}

// --- test clip ---------------------------------------------------------------

constexpr int kQuickFrames = 15;
constexpr uint32_t kQuickHoldMs = 1200;

// The whole clip in one mode. Each section is followed by the card held still, so
// ghosts have somewhere to show and settling has time to run. One `[result]` line
// per section, which is what to compare between modes.
void playClip(Mode mode, int hz, const char* only, int maxFrames, uint32_t holdMs) {
    const bool hybrid = usesEngine(mode);
    const char* name = kModeNames[(int)mode];
    if (!test_clip::render()) {
        Serial.println("[video] not enough PSRAM for the clip");
        return;
    }

    configure(mode);
    hardClear();
    const uint32_t savedSettle = engine.params().settleAfterMs;
    if (usesEngine(mode) && !settles(mode)) engine.params().settleAfterMs = 0;
    if (hybrid) engine.engage();

    const uint32_t periodMs = 1000 / hz;
    for (const test_clip::Section& s : test_clip::kSection) {
        if (g_stop) break;
        if (only && !strstr(only, s.name)) continue;
        Serial.printf("[video] %s / %s: %s\n", name, s.name, s.tests);
        const int frames = maxFrames && maxFrames < s.frames ? maxFrames : s.frames;
        const uint32_t cyclesBefore = painter.paintsCompleted();
        EpdHybrid::Counters before = {};
        if (hybrid) {
            engine.resetPeaks();
            before = engine.counters();
        }

        const uint32_t start = millis();
        uint32_t next = start, late = 0;
        for (int i = 0; i < frames && !stopRequested(); ++i) {
            test_clip::expand(s.first + i, frame);
            if (hybrid) engine.post(frame);
            else painter.paintPacked(frame);
            // Every frame finishes before the next starts, however late that makes it.
            if (oneAtATime(mode)) {
                while (!engine.quiet()) delay(1);
            }
            next += periodMs;
            const int32_t left = (int32_t)(next - millis());
            if (left > 0) pause(left);
            else late++;
        }
        const uint32_t took = millis() - start;

        test_clip::expandCard(frame);
        if (hybrid) {
            engine.post(frame);
            // Read before the card's own settle runs, or it counts against this.
            const EpdHybrid::Counters c = engine.counters();
            Serial.printf("[result] mode=%s section=%s hz=%d took_ms=%lu want_ms=%lu late=%lu "
                          "posted=%u accepted=%u refused=%u passes=%u overrun=%u floats=%u "
                          "slowest_pass_us=%lld slowest_submit_us=%lld worst_start_us=%lld "
                          "peak_cohorts=%d reversed=%u deferred=%u settles=%u scan_us=%lld\n",
                          name, s.name, hz, (unsigned long)took,
                          (unsigned long)(frames * periodMs), (unsigned long)late,
                          (unsigned)(c.posted - before.posted),
                          (unsigned)(c.accepted - before.accepted),
                          (unsigned)(c.refused - before.refused),
                          (unsigned)(c.passes - before.passes),
                          (unsigned)(c.overruns - before.overruns),
                          (unsigned)(c.floats - before.floats), (long long)c.slowestPassUs,
                          (long long)c.slowestSubmitUs, (long long)c.worstStartUs, c.peakCohorts,
                          (unsigned)(c.reversed - before.reversed),
                          (unsigned)(c.deferred - before.deferred),
                          (unsigned)(c.settles - before.settles), (long long)c.meanScanUs);
        } else {
            painter.paintPacked(frame);
            Serial.printf("[result] mode=%s section=%s hz=%d took_ms=%lu want_ms=%lu late=%lu "
                          "cycles=%u scan_us=%d\n",
                          name, s.name, hz, (unsigned long)took,
                          (unsigned long)(frames * periodMs), (unsigned long)late,
                          (unsigned)(painter.paintsCompleted() - cyclesBefore),
                          painter._config.rowloop_us);
        }
        pause(holdMs);
        if (hybrid) {
            for (int i = 0; i < 1000 && !engine.quiet(); ++i) delay(5);
        }
    }

    if (hybrid) engine.disengage();
    else while (!painter.paintIdle()) vTaskDelay(1);
    engine.params().settleAfterMs = savedSettle;
    Serial.printf("[video] %s done, card left on the glass\n", name);
}

// --- console -----------------------------------------------------------------

// Every knob on one line, for tuner.py.
void printState() {
    const hybrid::Params& p = engine.params();
    Serial.printf("[state] fast=%u k=%u settle=%lu period=%lld topup=%d paced=%d strict=%d "
                  "longgap=%d frames=%d hold=%lu rowextra=%d erase=%d\n",
                  p.fastPasses, p.settleK, (unsigned long)p.settleAfterMs,
                  (long long)engine.passUs, g_topUpPasses, g_pacedPassUs, g_strictPasses,
                  g_longGapUs, g_maxFrames, (unsigned long)g_holdMs, g_pacedRowUs, g_eraseK);
}

void help() {
    Serial.printf("[bench] last reset reason %d (3 restart, 4 panic, 5 int wdt, 6 task wdt, "
                  "9 brownout)\n",
                  g_resetReason);
    Serial.print("[bench] modes:");
    for (const char* m : kModeNames) Serial.printf(" %s", m);
    Serial.println();
    Serial.println("[bench] run <digits|pan|trail|flip> <mode> [hz]");
    Serial.println("[bench] video <mode|all> [hz] [section,section]");
    Serial.println("[bench] quick [hz]   shipping, fast, hybrid, shortened for filming");
    Serial.println("[bench] compare      fast, long, batch, hybrid on pan, strobe, cuts");
    Serial.println("[bench] paced        fast, paced, strict, batch on pan, strobe, cuts");
    Serial.println("[bench] set <fast|k|settle|period|topup|erase|paced|rowextra|strict|longgap|frames|hold> <value>");
    Serial.println("[bench] clear  state  stats  bootloader  help    x stops a run");
    printState();
}

void command(char* line) {
    const char* cmd = strtok(line, " ");
    if (!cmd) return;
    if (!strcmp(cmd, "run")) {
        const char* sceneName = strtok(nullptr, " ");
        const char* modeName = strtok(nullptr, " ");
        const char* hz = strtok(nullptr, " ");
        const Scene* scene = nullptr;
        for (const Scene& s : kScenes)
            if (sceneName && !strcmp(sceneName, s.name)) scene = &s;
        int mode = -1;
        for (int m = 0; m < kModes; ++m)
            if (modeName && !strcmp(modeName, kModeNames[m])) mode = m;
        if (!scene || mode < 0) return help();
        run(*scene, (Mode)mode, hz ? atoi(hz) : scene->hz);
    } else if (!strcmp(cmd, "quick")) {
        // For filming: every section, shortened. The hold is shorter than the
        // settle delay, so hybrid stays on each card until its settle has run.
        const char* hzArg = strtok(nullptr, " ");
        const int hz = hzArg ? atoi(hzArg) : 15;
        if (hz < 1) return help();
        const uint32_t start = millis();
        for (Mode m : {Mode::Shipping, Mode::Fast, Mode::Hybrid})
            playClip(m, hz, nullptr, kQuickFrames, kQuickHoldMs);
        Serial.printf("[video] quick done in %lu ms\n", (unsigned long)(millis() - start));
    } else if (!strcmp(cmd, "compare")) {
        // What is making hybrid clean: drive length (long), or cohorts (batch)?
        const uint32_t start = millis();
        for (Mode m : {Mode::Fast, Mode::Long, Mode::Batch, Mode::Hybrid})
            playClip(m, 15, "pan,strobe,cuts", kQuickFrames, kQuickHoldMs);
        Serial.printf("[video] compare done in %lu ms\n", (unsigned long)(millis() - start));
    } else if (!strcmp(cmd, "paced")) {
        // Is it this scan loop, or the 10 ms pass? And did skipped frames flatter batch?
        const uint32_t start = millis();
        for (Mode m : {Mode::Fast, Mode::Paced, Mode::Strict, Mode::Batch})
            playClip(m, 15, "pan,strobe,cuts", kQuickFrames, kQuickHoldMs);
        Serial.printf("[video] paced done in %lu ms\n", (unsigned long)(millis() - start));
    } else if (!strcmp(cmd, "video")) {
        const char* modeName = strtok(nullptr, " ");
        const char* hzArg = strtok(nullptr, " ");
        const char* only = strtok(nullptr, " ");
        const int hz = hzArg ? atoi(hzArg) : 15;
        if (hz < 1 || !modeName) return help();
        bool any = false;
        for (int m = 0; m < kModes && !g_stop; ++m) {
            if (!strcmp(modeName, "all") || !strcmp(modeName, kModeNames[m])) {
                playClip((Mode)m, hz, only, g_maxFrames, g_holdMs);
                any = true;
            }
        }
        if (!any) help();
    } else if (!strcmp(cmd, "set")) {
        const char* what = strtok(nullptr, " ");
        const char* val = strtok(nullptr, " ");
        if (!what || !val) return help();
        const long v = atol(val);
        hybrid::Params& p = engine.params();
        if (!strcmp(what, "fast") && v >= 1 && v <= hybrid::kMaxTrain) p.fastPasses = (uint8_t)v;
        else if (!strcmp(what, "k") && v >= 1 && v <= hybrid::kMaxTrain / 2) p.settleK = (uint8_t)v;
        else if (!strcmp(what, "settle") && v >= 0) p.settleAfterMs = (uint32_t)v;
        else if (!strcmp(what, "period") && v >= 6000) engine.passUs = v;
        else if (!strcmp(what, "topup") && v >= 0 && v <= hybrid::kMaxTrain) g_topUpPasses = (int)v;
        else if (!strcmp(what, "paced") && v >= 0) g_pacedPassUs = (int)v;
        else if (!strcmp(what, "rowextra") && v >= 0 && v <= 20) g_pacedRowUs = (int)v;
        else if (!strcmp(what, "erase") && v >= 0 && v <= 8) g_eraseK = (int)v;
        else if (!strcmp(what, "strict") && v >= 1 && v <= hybrid::kMaxTrain) g_strictPasses = (int)v;
        else if (!strcmp(what, "longgap") && v >= 0) g_longGapUs = (int)v;
        else if (!strcmp(what, "frames") && v >= 0) g_maxFrames = (int)v;
        else if (!strcmp(what, "hold") && v >= 0) g_holdMs = (uint32_t)v;
        else Serial.printf("[bench] set %s %ld: out of range\n", what, v);
        printState();
    } else if (!strcmp(cmd, "state")) {
        printState();
    } else if (!strcmp(cmd, "clear")) {
        hardClear();
    } else if (!strcmp(cmd, "stats")) {
        engine.printStats();
    } else if (!strcmp(cmd, "bootloader")) {
        Serial.flush();
        delay(100);
        usb_persist_restart(RESTART_BOOTLOADER);
    } else {
        help();
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    // Panics print to UART0, which is the GPS pin on this board, so this is the
    // only trace of why the last run ended.
    g_resetReason = (int)esp_reset_reason();
    frame =(uint8_t*)heap_caps_aligned_alloc(16, kPlaneBytes, MALLOC_CAP_SPIRAM);
    // Reset must mean reset here too: see the note in src/epd_compat.cpp.
    painter.setAutoShutdown(false);
    if (!frame || !painter.begin() || !engine.begin()) {
        Serial.println("[bench] init failed");
        for (;;) delay(1000);
    }
    g_normalGapUs = painter._config.pass_gap_us_normal;
    blank();
    hardClear();
    help();
}

void loop() {
    static char line[64];
    static size_t len = 0;
    while (Serial.available()) {
        const char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            line[len] = 0;
            len = 0;
            g_stop = false;
            command(line);
            Serial.println("[end]");   // tuner.py counts these
        } else if (len + 1 < sizeof line) {
            line[len++] = c;
        }
    }
    delay(5);
}
