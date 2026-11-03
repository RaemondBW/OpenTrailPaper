// Runs scan::Plan as the device's row loop would and checks what each pixel was sent.
// A pixel that changed must get exactly the train for its transition, and a pixel
// that did not must get nothing. Tables here are stand-ins with a distinct shape per
// transition, so a pixel on the wrong train cannot pass by accident.
#include <stdio.h>
#include <stdlib.h>

#include <vector>

#include "../../src/scan_core.h"

using namespace scan;

namespace {

int g_failures = 0;

uint8_t g_apply[3][13], g_remove[3][13], g_direct[4][4][26];

void makeTables() {
    for (int g = 0; g < 3; ++g) {
        for (int i = 0; i < 13; ++i) {
            g_apply[g][i] = (i + g) % 3 == 2 ? 0 : 1;
            g_remove[g][i] = (i + g) % 4 == 0 ? 1 : 2;
        }
    }
    g_apply[0][1] = 3;   // a code 3, which must come out as a float
    for (int f = 1; f <= 3; ++f) {
        for (int t = 1; t <= 3; ++t) {
            if (f == t) continue;
            for (int i = 0; i < 26; ++i) g_direct[f][t][i] = (i * (f + 1) + t) % 3;
            g_direct[f][t][20 + t] = 2;
        }
    }
}

struct Rig {
    Plan plan;
    std::vector<uint8_t> frame = std::vector<uint8_t>(kPlaneBytes, 0);
    std::vector<uint8_t> screen = std::vector<uint8_t>(kPlaneBytes, 0);
    std::vector<std::vector<uint8_t>> planes;
    std::vector<std::vector<uint32_t>> masks;

    Rig() {
        plan.build({g_apply, g_remove, g_direct}, 7, 2);
        for (int p = 0; p < kPlanes; ++p) {
            planes.emplace_back(kPlaneBytes, 0);
            masks.emplace_back(kHeight, 0);
        }
    }
    static int get(const std::vector<uint8_t>& b, int x, int y) {
        return (b[(size_t)y * kRowBytes + (x >> 2)] >> (6 - 2 * (x & 3))) & 3;
    }
    void set(int x, int y, int level) {
        uint8_t& b = frame[(size_t)y * kRowBytes + (x >> 2)];
        const int shift = 6 - 2 * (x & 3);
        b = (uint8_t)((b & ~(3 << shift)) | (level << shift));
    }

    // One drive from `screen` to `frame`, checking every pixel. Returns the passes.
    int drive(const char* name) {
        const std::vector<uint8_t> before = screen;
        uint8_t* p[kPlanes];
        uint32_t* m[kPlanes];
        for (int i = 0; i < kPlanes; ++i) {
            p[i] = planes[i].data();
            m[i] = masks[i].data();
        }
        const int passes = plan.discover(frame.data(), screen.data(), p, m);

        int wrong = 0, moved = 0, longest = 0;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const int from = get(before, x, y), to = get(frame, x, y);
                const Train& want = plan.train(from, to);
                if (from != to) {
                    moved++;
                    if (want.len > longest) longest = want.len;
                }
                for (int pass = 0; pass < passes; ++pass) {
                    int code = kFloat, seen = 0;
                    for (int pl = 0; pl < kPlanes; ++pl) {
                        const int slot = get(planes[pl], x, y);
                        if (!slot) continue;
                        seen++;
                        if (!(masks[pl][y] & (0x80000000u >> ((x >> 2) / kChunkBytes)))) wrong++;
                        code = plan.code(pl, slot, pass);
                    }
                    const int expected = from != to && pass < want.len ? want.code[pass] : kFloat;
                    if (code != expected || seen > 1) wrong++;
                }
            }
        }
        if (screen != frame) wrong++;
        const bool ok = !wrong && passes == longest;
        printf("%s %-30s %d moved, %d passes, %d wrong\n", ok ? "ok  " : "FAIL", name, moved, passes, wrong);
        if (!ok) g_failures++;
        for (auto& pl : planes) std::fill(pl.begin(), pl.end(), 0);
        for (auto& ma : masks) std::fill(ma.begin(), ma.end(), 0);
        return passes;
    }
};

void trains() {
    Rig r;
    const Train& erase = r.plan.train(3, 0);
    int net = 0;
    for (int i = 0; i < erase.len; ++i) net += erase.code[i] == kDarken ? 1 : -1;
    const bool ok = r.plan.complete() && erase.len == 11 && erase.code[0] == kDarken &&
                    erase.code[1] == kDarken && erase.code[2] == kWhiten && net == -7 &&
                    r.plan.train(0, 3).len == 7 && r.plan.train(0, 1).code[1] == kFloat;
    printf("%s %-30s erase %d passes, net %d\n", ok ? "ok  " : "FAIL", "trains", erase.len, net);
    if (!ok) g_failures++;
}

void binaryFrames() {
    Rig r;
    for (int y = 100; y < 200; ++y)
        for (int x = 100; x < 300; ++x) r.set(x, y, 3);
    if (r.drive("black on white") != 7) g_failures++;
    if (r.drive("same frame again") != 0) g_failures++;
    for (int y = 100; y < 200; ++y)
        for (int x = 100; x < 300; ++x) r.set(x, y, 0);
    if (r.drive("back to white") != 11) g_failures++;
    if (!r.plan.planeInUse(0) || r.plan.planeInUse(1)) {
        printf("FAIL a black and white frame used more than its one plane\n");
        g_failures++;
    }
}

void everyTransition() {
    Rig r;
    // Lay every level down, then move each to every other in stripes that share chunks.
    for (int to = 0; to < 4; ++to) {
        for (int from = 0; from < 4; ++from) {
            for (int y = 0; y < kHeight; ++y)
                for (int x = 0; x < kWidth; ++x) r.set(x, y, (x + y) % 4 == 0 ? from : 0);
            r.drive("to the start level");
            for (int y = 0; y < kHeight; ++y)
                for (int x = 0; x < kWidth; ++x) r.set(x, y, (x + y) % 4 == 0 ? to : 0);
            char name[32];
            snprintf(name, sizeof name, "%d to %d", from, to);
            r.drive(name);
        }
    }
}

void mixedSoak() {
    Rig r;
    srand(99);
    for (int i = 0; i < 12; ++i) {
        for (int k = 0; k < 30; ++k) {
            const int x0 = rand() % 900, y0 = rand() % 500, level = rand() % 4;
            for (int y = y0; y < y0 + 30 && y < kHeight; ++y)
                for (int x = x0; x < x0 + 50 && x < kWidth; ++x) r.set(x, y, level);
        }
        r.drive("random levels");
    }
}

void missingTable() {
    Rig r;
    static uint8_t empty[4][4][26];
    r.plan.build({g_apply, g_remove, empty}, 7, 2);
    const bool ok = !r.plan.complete();
    printf("%s %-30s\n", ok ? "ok  " : "FAIL", "an unloaded table is caught");
    if (!ok) g_failures++;
}

}  // namespace

int main() {
    makeTables();
    trains();
    binaryFrames();
    everyTransition();
    mixedSoak();
    missingTable();
    printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
