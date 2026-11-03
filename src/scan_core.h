// Which pixel gets which waveform, for the panel scan in epd_scan.cpp.
//
// Black and white take the fast path: a plain darken run, and for black to white
// the same run primed with a couple of darkens and matched by extra whitens, which
// is what erases cleanly and keeps the charge balanced. Everything involving a grey
// takes the driver's tuned 4-level trains, which are longer but rare: the UI only
// uses greys for static art. See investigations/modos-hybrid-update.md.
//
// Hardware-free, so tools/scan_test can run it on the host.
#pragma once

#include <stdint.h>
#include <string.h>

namespace scan {

constexpr int kWidth = 960;
constexpr int kHeight = 540;
constexpr int kRowBytes = kWidth / 4;   // 2bpp, first pixel in the MSBs
constexpr int kChunkBytes = 16;         // what the driver's convert kernel works in
constexpr int kChunks = kRowBytes / kChunkBytes;
constexpr size_t kPlaneBytes = (size_t)kRowBytes * kHeight;

// A plane's pixels hold slot 0 (float) or 1..3, and each slot names one transition.
// Twelve transitions exist and the two black and white ones get a plane to
// themselves so the common frame only ever costs one sweep.
constexpr int kPlanes = 5;
constexpr int kMaxTrain = 26;

constexpr uint8_t kFloat = 0, kDarken = 1, kWhiten = 2;

struct Train {
    uint8_t len = 0;
    uint8_t code[kMaxTrain] = {};
};

// The driver's tables for one board. All are level-indexed, level 0 = white.
struct Tables {
    const uint8_t (*apply)[13];       // [level - 1]: white to level
    const uint8_t (*remove)[13];      // [level - 1]: level to white
    const uint8_t (*direct)[4][26];   // [from][to], between non-white levels
};

class Plan {
public:
    // `passes` is the black and white drive, `prime` the erase priming.
    void build(const Tables& t, uint8_t passes, uint8_t prime) {
        memset(train_, 0, sizeof train_);
        train_[0][3] = run(kDarken, passes);
        train_[3][0] = erase(passes, prime);
        for (int g = 1; g <= 2; ++g) {
            train_[0][g] = fromTable(t.apply[g - 1], 13);
            train_[g][0] = fromTable(t.remove[g - 1], 13);
        }
        for (int from = 1; from <= 3; ++from) {
            for (int to = 1; to <= 3; ++to) {
                if (from != to) train_[from][to] = fromTable(t.direct[from][to], kMaxTrain);
            }
        }
    }

    // Every transition has a train. A false here means a table the driver never
    // loaded, and that transition would silently do nothing.
    bool complete() const {
        for (int from = 0; from < 4; ++from) {
            for (int to = 0; to < 4; ++to) {
                if (from != to && !train_[from][to].len) return false;
            }
        }
        return true;
    }

    const Train& train(int from, int to) const { return train_[from][to]; }

    // The drive code for `slot` of `plane` on `pass`.
    uint8_t code(int plane, int slot, int pass) const {
        const Slot& s = kSlots[plane][slot];
        const Train& t = train_[s.from][s.to];
        return pass < t.len ? t.code[pass] : kFloat;
    }

    int planeInUse(int plane) const { return used_[plane]; }

    // Fill the planes with what it takes to get from `screen` to `frame`, and record
    // the result in `screen`. Planes must be zero, masks are one word per row (bit 31
    // = leftmost chunk). Returns how many passes the frame needs, 0 if nothing moved.
    int discover(const uint8_t* frame, uint8_t* screen, uint8_t* const* planes,
                 uint32_t* const* masks) {
        memset(used_, 0, sizeof used_);
        uint8_t present[4] = {};   // bit per `to`, indexed by `from`
        for (int row = 0; row < kHeight; ++row) {
            for (int chunk = 0; chunk < kChunks; ++chunk) {
                const size_t at = (size_t)row * kRowBytes + chunk * kChunkBytes;
                uint32_t want[4], have[4];
                memcpy(want, frame + at, kChunkBytes);
                memcpy(have, screen + at, kChunkBytes);
                if (!memcmp(want, have, kChunkBytes)) continue;

                uint32_t out[kPlanes][4] = {};
                uint8_t hit = 0;
                for (int w = 0; w < 4; ++w) {
                    const uint32_t changed = want[w] ^ have[w];
                    if (!changed) continue;
                    if (binary(want[w]) && binary(have[w])) {
                        // Both bits of a pixel agree, so this is every pixel at once.
                        const uint32_t moved = (changed | (changed >> 1)) & kLow;
                        const uint32_t black = moved & want[w];
                        const uint32_t white = moved & ~want[w];
                        out[0][w] = black | (white << 1);
                        if (black) { hit |= 1; present[0] |= 1 << 3; }
                        if (white) { hit |= 1; present[3] |= 1 << 0; }
                        continue;
                    }
                    for (int px = 0; px < 16; ++px) {
                        const int from = (have[w] >> (2 * px)) & 3;
                        const int to = (want[w] >> (2 * px)) & 3;
                        if (from == to) continue;
                        const Place& p = kPlaces[from][to];
                        out[p.plane][w] |= (uint32_t)p.slot << (2 * px);
                        hit |= (uint8_t)(1 << p.plane);
                        present[from] |= (uint8_t)(1 << to);
                    }
                }

                for (int p = 0; p < kPlanes; ++p) {
                    if (!(hit & (1 << p))) continue;
                    memcpy(planes[p] + at, out[p], kChunkBytes);
                    masks[p][row] |= 0x80000000u >> chunk;
                    used_[p] = 1;
                }
                memcpy(screen + at, want, kChunkBytes);
            }
        }

        int passes = 0;
        for (int from = 0; from < 4; ++from) {
            for (int to = 0; to < 4; ++to) {
                if ((present[from] & (1 << to)) && train_[from][to].len > passes)
                    passes = train_[from][to].len;
            }
        }
        return passes;
    }

private:
    struct Slot { uint8_t from, to; };
    struct Place { uint8_t plane, slot; };

    static constexpr uint32_t kLow = 0x55555555;

    // Slot order within plane 0 matters: discover() writes 01 for black, 10 for white.
    static constexpr Slot kSlots[kPlanes][4] = {
        {{0, 0}, {0, 3}, {3, 0}, {0, 0}},
        {{0, 0}, {0, 1}, {0, 2}, {1, 0}},
        {{0, 0}, {2, 0}, {1, 2}, {2, 1}},
        {{0, 0}, {1, 3}, {3, 1}, {2, 3}},
        {{0, 0}, {3, 2}, {0, 0}, {0, 0}},
    };
    static constexpr Place kPlaces[4][4] = {
        {{0, 0}, {1, 1}, {1, 2}, {0, 1}},
        {{1, 3}, {0, 0}, {2, 2}, {3, 1}},
        {{2, 1}, {2, 3}, {0, 0}, {3, 3}},
        {{0, 2}, {3, 2}, {4, 1}, {0, 0}},
    };

    // Every pixel in the word is 00 or 11.
    static bool binary(uint32_t w) { return !((w ^ (w >> 1)) & kLow); }

    static Train run(uint8_t code, uint8_t len) {
        Train t;
        t.len = len;
        memset(t.code, code, len);
        return t;
    }

    static Train erase(uint8_t passes, uint8_t prime) {
        Train t;
        t.len = (uint8_t)(passes + 2 * prime);
        memset(t.code, kDarken, prime);
        memset(t.code + prime, kWhiten, passes + prime);
        return t;
    }

    // Code 3 is both electrodes at once: no net drive for real current, so the driver
    // floats it, and its tables were tuned with that. Trailing floats are dropped so
    // a short train does not pad the frame.
    static Train fromTable(const uint8_t* codes, int len) {
        Train t;
        for (int i = 0; i < len; ++i) {
            t.code[i] = codes[i] == 3 ? kFloat : codes[i];
            if (t.code[i]) t.len = (uint8_t)(i + 1);
        }
        return t;
    }

    Train train_[4][4];
    int used_[kPlanes] = {};
};

inline constexpr Plan::Slot Plan::kSlots[kPlanes][4];
inline constexpr Plan::Place Plan::kPlaces[4][4];

}  // namespace scan
