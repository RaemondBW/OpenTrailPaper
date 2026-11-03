// Runs hybrid::Core against a model of the glass. Each pixel is a position
// between white (0) and black (kFull) that a darken pass moves up and a whiten
// pass moves down, plus a ledger of the charge it has been given, which is not
// clamped. Two things must hold once the core goes quiet, whatever the sequence
// of frames: every pixel sits on the rail it was asked for, and its ledger is
// exactly what one clean trip there would have left. The second one is DC
// balance, and it is the one that can only be got wrong silently on real glass.
#include <stdio.h>
#include <stdlib.h>

#include <vector>

#include "hybrid_core.h"

using namespace hybrid;

namespace {

constexpr int kPasses = 8;
int g_failures = 0;

struct Rig {
    Core* core = new Core;
    std::vector<uint8_t*> planes;
    std::vector<uint8_t> frame = std::vector<uint8_t>(kPlaneBytes, 0);
    std::vector<int> position = std::vector<int>(kWidth * kHeight, 0);
    std::vector<int> ledger = std::vector<int>(kWidth * kHeight, 0);
    uint32_t dirty[kHeight];
    uint32_t now = 0;

    explicit Rig(uint32_t settleMs) {
        for (int i = 0; i < kCohorts + kGenerations + 2; ++i)
            planes.push_back((uint8_t*)calloc(1, kPlaneBytes));
        core->begin(planes.data());
        core->params.fastPasses = kPasses;
        core->params.settleK = 3;
        core->params.settleAfterMs = settleMs;
        core->reset(0);
        for (uint32_t& d : dirty) d = 0xFFFE0000;   // all 15 chunks
    }
    ~Rig() {
        for (uint8_t* p : planes) free(p);
        delete core;
    }

    void set(int x, int y, int level) {
        uint8_t& b = frame[(size_t)y * kRowBytes + (x >> 2)];
        const int shift = 6 - 2 * (x & 3);
        b = (uint8_t)((b & ~(3 << shift)) | (level << shift));
    }
    void rect(int x0, int y0, int w, int h, int level) {
        for (int y = y0; y < y0 + h; ++y)
            for (int x = x0; x < x0 + w; ++x) set(x, y, level);
    }
    int level(int x, int y) const {
        return (frame[(size_t)y * kRowBytes + (x >> 2)] >> (6 - 2 * (x & 3))) & 3;
    }

    bool submit() { return core->submit(frame.data(), dirty, now); }

    // One pass on the wire, as the device's row loop would drive it. A pixel in
    // two cohorts at once would be a real short-dose bug, so that fails too.
    void pass() {
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                int seen = 0, code = kFloat;
                for (int i = 0; i < kCohorts; ++i) {
                    const Cohort& c = core->cohort(i);
                    if (c.kind == Kind::Free) continue;
                    const int slot = (c.plane[(size_t)y * kRowBytes + (x >> 2)] >>
                                      (6 - 2 * (x & 3))) & 3;
                    if (!slot) continue;
                    // A set slot outside the chunk mask would never be driven.
                    if (!(c.mask[y] & chunkBit((x >> 2) / kChunkBytes))) {
                        printf("  slot outside mask at %d,%d\n", x, y);
                        g_failures++;
                    }
                    seen++;
                    code = Core::code(c, slot);
                }
                if (seen > 1) {
                    printf("  pixel %d,%d in %d cohorts\n", x, y, seen);
                    g_failures++;
                }
                int& pos = position[(size_t)y * kWidth + x];
                if (code == kDarken) {
                    if (pos < kPasses) pos++;
                    ledger[(size_t)y * kWidth + x]++;
                } else if (code == kWhiten) {
                    if (pos > 0) pos--;
                    ledger[(size_t)y * kWidth + x]--;
                }
            }
        }
        core->advance();
        now += 10;
        core->age(now);
    }

    void runQuiet() {
        for (int i = 0; i < 2000 && !core->idle(); ++i) pass();
    }

    void check(const char* name) {
        int wrongLanding = 0, wrongLedger = 0;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const int want = level(x, y) >= 2 ? kPasses : 0;
                if (position[(size_t)y * kWidth + x] != want) wrongLanding++;
                if (ledger[(size_t)y * kWidth + x] != want) wrongLedger++;
            }
        }
        const bool ok = !wrongLanding && !wrongLedger && core->idle();
        printf("%s %-28s landing errors %d, ledger errors %d, idle %d\n", ok ? "ok  " : "FAIL",
               name, wrongLanding, wrongLedger, (int)core->idle());
        if (!ok) g_failures++;
    }
};

void singleUpdate() {
    Rig r(0);
    r.rect(100, 100, 200, 100, 3);
    r.submit();
    r.runQuiet();
    r.check("single update");
    r.rect(100, 100, 200, 100, 0);
    r.submit();
    r.runQuiet();
    r.check("single update, erased");
}

// The point of cohorts: a second region starts while the first is mid-drive and
// finishes on its own schedule.
void overlapping() {
    Rig r(0);
    r.rect(0, 0, 64, 64, 3);
    r.submit();
    r.pass();
    r.pass();
    r.pass();
    r.rect(200, 200, 64, 64, 3);
    r.submit();
    if (r.position[(size_t)210 * kWidth + 210] != 0 || r.position[10] != 3) {
        printf("  second region should start from zero while the first is at 3\n");
        g_failures++;
    }
    r.runQuiet();
    r.check("overlapping regions");
}

// White to black, reversed at every possible point. In the worst case the pixel
// is one pass from done, and it must still come back white with a zero ledger.
void cancellation() {
    for (int elapsed = 0; elapsed < kPasses; ++elapsed) {
        Rig r(0);
        r.rect(320, 200, 128, 64, 3);
        r.submit();
        for (int i = 0; i < elapsed; ++i) r.pass();
        r.rect(320, 200, 128, 64, 0);
        r.submit();
        r.runQuiet();
        char name[40];
        snprintf(name, sizeof name, "cancel after %d passes", elapsed);
        r.check(name);
    }
}

// Changing a pixel again while its reversal runs defers it, and it must still
// reach the newest frame with nobody submitting anything further.
void changeDuringReversal() {
    Rig r(0);
    r.rect(0, 0, 32, 32, 3);
    r.submit();
    r.pass();
    r.pass();
    r.pass();
    r.pass();
    r.rect(0, 0, 32, 32, 0);
    r.submit();
    r.pass();
    r.rect(0, 0, 32, 32, 3);
    r.submit();
    r.runQuiet();
    r.check("change during a reversal");
    if (!r.core->stats().deferred) {
        printf("  expected a deferral\n");
        g_failures++;
    }
}

void settleLeavesPixelsWhereTheyWere() {
    Rig r(100);
    r.rect(64, 64, 128, 128, 3);
    r.submit();
    r.pass();
    r.pass();
    r.runQuiet();
    r.check("settle after a black fill");
    if (r.core->stats().settles != 1) {
        printf("  expected one settle, got %u\n", (unsigned)r.core->stats().settles);
        g_failures++;
    }
    r.rect(64, 64, 128, 128, 0);
    r.submit();
    r.runQuiet();
    r.check("settle after erasing it");
}

// Settle applies only to the pixels that moved. Nothing else may be driven at
// all: that is the whole advantage over the rectangular scrub.
void settleIsPixelExact() {
    Rig r(100);
    for (int y = 0; y < 128; ++y)
        for (int x = (y & 1); x < 128; x += 2) r.set(x, y, 3);   // checkerboard
    r.submit();
    r.runQuiet();
    int touched = 0;
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x)
            if (r.level(x, y) < 2 && r.ledger[(size_t)y * kWidth + x] != 0) touched++;
    printf("%s %-28s %d unchanged pixels driven\n", touched ? "FAIL" : "ok  ",
           "settle pixel exact", touched);
    if (touched) g_failures++;
    r.check("checkerboard settle");
}

// A pixel still being changed must never settle: it has not stopped moving.
void busyPixelsDoNotSettle() {
    Rig r(100);
    for (int i = 0; i < 12; ++i) {
        r.rect(0, 0, 32, 32, (i & 1) ? 0 : 3);
        r.submit();
        for (int p = 0; p < 6; ++p) r.pass();
    }
    const uint32_t settlesDuring = r.core->stats().settles;
    r.runQuiet();
    r.check("busy pixels");
    if (settlesDuring) {
        printf("  %u settles while the pixels were still changing\n", (unsigned)settlesDuring);
        g_failures++;
    }
}

// Faster than the cohorts can retire: frames get refused rather than dropped
// pixels, and the newest one still lands.
void poolExhaustion() {
    Rig r(0);
    int refused = 0;
    for (int i = 0; i < 40; ++i) {
        r.rect((i * 40) % 880, (i * 37) % 480, 32, 32, 3);
        if (!r.submit()) refused++;
        if (i % 2) r.pass();
    }
    while (!r.submit()) r.pass();
    r.runQuiet();
    r.check("cohort pool exhausted");
    printf("     (%d refused, peak %d cohorts)\n", refused, (int)r.core->stats().peakCohorts);
}

// Priming must not change where a pixel lands or what charge it ends with: the whole
// point of doing it this way round is that the ledger still closes.
void erasePrimingStaysBalanced() {
    Rig r(0);
    r.core->params.eraseK = 2;
    r.rect(100, 100, 200, 100, 3);
    r.submit();
    r.runQuiet();
    r.check("priming, drawn");
    r.rect(100, 100, 200, 100, 0);
    r.rect(400, 300, 100, 50, 3);    // and something drawing in the same frame
    r.submit();
    r.runQuiet();
    r.check("priming, erased");
}

// The new pixels drive as normal, and the black pixels already there get the top-up
// instead of nothing, which is the point. Pixels in chunks with no change: nothing.
void topUpEvensOutTheBlock() {
    Rig r(0);
    r.core->params.fastPasses = kPasses;
    r.core->params.topUp = 3;
    for (int y = 100; y < 300; ++y) r.rect(320, y, 4, 1, 3);   // a line
    r.submit();
    r.runQuiet();
    r.rect(256, 150, 256, 64, 3);                              // the block over it
    r.submit();
    r.runQuiet();
    int wrong = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (r.position[(size_t)y * kWidth + x] != (r.level(x, y) >= 2 ? kPasses : 0)) wrong++;
        }
    }
    const int under = r.ledger[(size_t)160 * kWidth + 321];    // line, under the block
    const int beside = r.ledger[(size_t)160 * kWidth + 300];   // block, newly black
    const int above = r.ledger[(size_t)120 * kWidth + 321];    // line, nothing near it
    const int blank = r.ledger[(size_t)400 * kWidth + 800];
    const bool ok = !wrong && under == kPasses + 3 && beside == kPasses &&
                    above == kPasses && blank == 0 && r.core->idle();
    printf("%s %-28s %d landing errors; under %d, beside %d, above %d, blank %d\n",
           ok ? "ok  " : "FAIL", "top-up evens out the block", wrong, under, beside, above,
           blank);
    if (!ok) g_failures++;
}

// Everything at once, from a fixed seed.
void soak() {
    for (uint32_t settleMs : {0u, 100u}) {
        Rig r(settleMs);
        srand(1234 + settleMs);
        for (int i = 0; i < 400; ++i) {
            r.rect(rand() % 800, rand() % 450, 8 + rand() % 120, 4 + rand() % 80,
                   (rand() & 1) ? 3 : 0);
            while (!r.submit()) r.pass();
            for (int p = rand() % 9; p > 0; --p) r.pass();
        }
        r.runQuiet();
        r.check(settleMs ? "soak with settle" : "soak");
        const Stats& s = r.core->stats();
        printf("     (%u frames, %u reversed words, %u deferred, %u settles, peak %d)\n",
               (unsigned)s.frames, (unsigned)s.cancelled, (unsigned)s.deferred,
               (unsigned)s.settles, (int)s.peakCohorts);
    }
}

}  // namespace

int main() {
    singleUpdate();
    overlapping();
    cancellation();
    changeDuringReversal();
    settleLeavesPixelsWhereTheyWere();
    settleIsPixelExact();
    busyPixelsDoNotSettle();
    poolExhaustion();
    erasePrimingStaysBalanced();
    topUpEvensOutTheBlock();
    soak();
    printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
