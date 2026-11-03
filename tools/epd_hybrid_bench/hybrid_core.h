// Modos-style hybrid e-paper update, without per-pixel counters.
//
// Caster (the Modos FPGA controller) gives every pixel its own timer, which
// costs 16 bits per pixel, every frame. We have neither the memory bandwidth nor
// the cycles for that, but we do not need it: a pixel only starts moving when a
// frame is submitted, so all the pixels of one submission share a timer. A
// handful of overlapping COHORTS (one slot plane + one pass counter each) gives
// the same three behaviours:
//
//   independent updates  a new frame starts driving at the next pass instead of
//                        waiting for the previous one to finish
//   early cancellation   a pixel reversed mid-drive runs its elapsed passes
//                        backwards, so it lands where it started, DC-neutral
//   hybrid settle        changed pixels go fast and binary; once a pixel has been
//                        stable for a while it alone gets a cleaning train, which
//                        replaces the rectangular scrub without touching the
//                        pixels around it
//
// Everything here is binary: level 2 and 3 are black, 0 and 1 are white. The
// greyscale half of Caster's hybrid mode is not implemented yet.
//
// No hardware in this file, so tools/epd_hybrid_bench/run_core_test.sh can check
// landings and the DC ledger on the host. See
// investigations/modos-hybrid-update.md.
#pragma once

#include <stdint.h>
#include <string.h>

namespace hybrid {

constexpr int kWidth = 960;
constexpr int kHeight = 540;
constexpr int kRowBytes = kWidth / 4;   // 2bpp, first pixel in the MSBs
constexpr int kChunkBytes = 16;         // what the convert kernels work in
constexpr int kChunks = kRowBytes / kChunkBytes;
constexpr size_t kPlaneBytes = (size_t)kRowBytes * kHeight;

constexpr int kMaxTrain = 32;
constexpr int kCohorts = 8;
constexpr int kGenerations = 3;

// The driver's drive codes.
constexpr uint8_t kFloat = 0, kDarken = 1, kWhiten = 2;

// A pixel's slot within its cohort. Slot 0 floats, which is what keeps the OR of
// several cohorts into one row safe: a pixel is in at most one of them.
constexpr uint32_t kSlotBlack = 0x55555555;   // 01: heading for black
constexpr uint32_t kSlotWhite = 0xAAAAAAAA;   // 10: heading for white

enum class Kind : uint8_t { Free, Fast, Reverse, Settle };

struct Train {
    uint8_t len = 0;
    uint8_t code[kMaxTrain] = {};
};

struct Cohort {
    Kind kind = Kind::Free;
    uint8_t pass = 0;
    uint8_t len = 0;
    Train toBlack, toWhite;
    uint8_t* plane = nullptr;       // kPlaneBytes of 2bpp slots
    uint32_t mask[kHeight] = {};    // chunk bits, bit 31 = leftmost
};

struct Params {
    uint8_t fastPasses = 8;         // length of the binary drive
    uint8_t settleK = 3;            // settle train is k of one, then k of the other
    uint32_t settleAfterMs = 1500;  // 0 turns settling off
    // Priming for black to white: this many darkens first, then that many extra
    // whitens on top of the usual run. A whiten is much stronger from a deeper black,
    // which is what the driver's tuned removes found, and an erase of plain whitens
    // stops short of paper white. Net charge is unchanged, and the pixel is already
    // black while it darkens, so nothing shows. Costs 2 x this many passes.
    uint8_t eraseK = 0;
    // Passes of top-up for pixels that did not change but share a chunk with one
    // that did: black driven blacker, white whiter, at the end of the drive. Only
    // changed pixels get a drive otherwise, so a black pixel beside a fresh one has a
    // different history and shows. Not DC-balanced: see the investigation note.
    // Assumes one frame at a time (EpdHybrid::batch). 0 turns it off.
    uint8_t topUp = 0;
};

struct Stats {
    uint32_t frames = 0;            // submissions accepted
    uint32_t refused = 0;           // submissions turned away: no free cohort
    uint32_t cancelled = 0;         // chunks words holding a reversed pixel
    uint32_t deferred = 0;          // words deferred past a locked pixel
    uint32_t settles = 0;           // settle cohorts started
    uint8_t peakCohorts = 0;
};

// Every pixel a slot word says is occupied, as 11.
inline uint32_t occupied(uint32_t slots) {
    const uint32_t s = (slots | (slots >> 1)) & kSlotBlack;
    return s | (s << 1);
}

inline uint32_t chunkBit(int chunk) { return 0x80000000u >> chunk; }

class Core {
public:
    // Planes come from the caller so the host test can use malloc and the device
    // aligned PSRAM. Needs kCohorts + kGenerations + 2 planes, zeroed.
    void begin(uint8_t* const* planes) {
        int n = 0;
        for (Cohort& c : cohorts_) c.plane = planes[n++];
        for (Gen& g : gens_) g.plane = planes[n++];
        committed_ = planes[n++];
        last_ = planes[n++];
    }

    // The glass was just cleared to white behind our back.
    void reset(uint32_t nowMs) {
        for (Cohort& c : cohorts_) {
            memset(c.plane, 0, kPlaneBytes);
            memset(c.mask, 0, sizeof c.mask);
            c.kind = Kind::Free;
        }
        for (Gen& g : gens_) {
            memset(g.plane, 0, kPlaneBytes);
            memset(g.mask, 0, sizeof g.mask);
        }
        memset(committed_, 0, kPlaneBytes);
        memset(last_, 0, kPlaneBytes);
        memset(deferred_, 0, sizeof deferred_);
        anyDeferred_ = false;
        genCur_ = 0;
        genSince_ = nowMs;
        stats_ = Stats{};
    }

    Params params;

    const Stats& stats() const { return stats_; }
    void resetPeak() { stats_.peakCohorts = 0; }
    const Cohort& cohort(int i) const { return cohorts_[i]; }
    bool driving() const {
        for (const Cohort& c : cohorts_)
            if (c.kind != Kind::Free) return true;
        return false;
    }
    // Work is still owed even though nothing is on the wire right now.
    bool idle() const { return !driving() && !anyDeferred_ && !genDirty(); }

    // The drive code cohort `c` puts on `slot` (1 black, 2 white) this pass.
    static uint8_t code(const Cohort& c, int slot) {
        const Train& t = slot == 1 ? c.toBlack : c.toWhite;
        return c.pass < t.len ? t.code[c.pass] : kFloat;
    }

    // Take a frame. `dirty` is a chunk mask per row saying where it can differ
    // from the last one. Refused (nothing changed, try again) when there are not
    // enough free cohorts for the worst case, so a submission is all or nothing.
    bool submit(const uint8_t* frame, const uint32_t* dirty, uint32_t nowMs) {
        int free = 0, reversible = 0;
        for (const Cohort& c : cohorts_) {
            if (c.kind == Kind::Free) free++;
            if (c.kind == Kind::Fast && c.pass > 0) reversible++;
        }
        if (free < 1 + reversible + (params.topUp ? 1 : 0)) {
            stats_.refused++;
            return false;
        }

        Cohort* fresh = claim(Kind::Fast);
        fresh->toBlack = solid(kDarken, params.fastPasses);
        fresh->toWhite = erase(params.fastPasses, params.eraseK);
        fresh->len = fresh->toWhite.len;
        Cohort* reverseOf[kCohorts] = {};
        Cohort* hold = nullptr;
        if (params.topUp) {
            hold = claim(Kind::Settle);
            const uint8_t n = params.fastPasses, k = params.topUp < n ? params.topUp : n;
            hold->toBlack = Train{};
            hold->toWhite = Train{};
            hold->toBlack.len = hold->toWhite.len = n;
            hold->len = n;
            memset(hold->toBlack.code + (n - k), kDarken, k);
            memset(hold->toWhite.code + (n - k), kWhiten, k);
        }

        Gen& cur = gens_[genCur_];
        bool work = false;
        for (int row = 0; row < kHeight; ++row) {
            uint32_t todo = dirty[row];
            for (int chunk = 0; todo && chunk < kChunks; ++chunk) {
                const uint32_t bit = chunkBit(chunk);
                if (!(todo & bit)) continue;
                todo &= ~bit;
                const size_t at = (size_t)row * kRowBytes + chunk * kChunkBytes;
                if (frame != last_) memcpy(last_ + at, frame + at, kChunkBytes);
                bool moved = false;
                for (int w = 0; w < 4; ++w) {
                    if (submitWord(frame, at + w * 4, row, bit, fresh, reverseOf, cur))
                        moved = true;
                }
                work |= moved;
                if (moved && hold) holdChunk(*hold, *fresh, at, row, bit);
            }
        }

        if (!work) release(*fresh);
        if (!work && hold) release(*hold);
        stats_.frames++;
        int inUse = 0;
        for (const Cohort& c : cohorts_) inUse += c.kind != Kind::Free;
        if (inUse > stats_.peakCohorts) stats_.peakCohorts = (uint8_t)inUse;
        (void)nowMs;
        return true;
    }

    // One pass has been driven.
    void advance() {
        bool unlocked = false;
        for (Cohort& c : cohorts_) {
            if (c.kind == Kind::Free) continue;
            if (++c.pass < c.len) continue;
            unlocked |= c.kind != Kind::Fast;
            release(c);
        }
        if (unlocked && anyDeferred_) retryDeferred();
    }

    // Age the generations. A pixel that changed sits in the current generation
    // and is struck out of it if it changes again; whatever is left in the oldest
    // when it comes due has been stable for at least settleAfterMs.
    void age(uint32_t nowMs) {
        if (anyDeferred_) retryDeferred();
        if (!params.settleAfterMs) return;
        if (nowMs - genSince_ < params.settleAfterMs / (kGenerations - 1)) return;
        const int oldest = (genCur_ + 1) % kGenerations;
        Gen& g = gens_[oldest];
        if (genDirty(g)) {
            Cohort* c = claim(Kind::Settle);
            if (!c) return;   // the rotation just waits for a free cohort
            startSettle(*c, g);
        }
        genCur_ = oldest;
        genSince_ = nowMs;
    }

private:
    struct Gen {
        uint8_t* plane = nullptr;
        uint32_t mask[kHeight] = {};
    };

    static Train solid(uint8_t code, uint8_t len) {
        Train t;
        t.len = len;
        memset(t.code, code, len);
        return t;
    }

    static Train erase(uint8_t passes, uint8_t prime) {
        if (2 * prime + passes > kMaxTrain) prime = (uint8_t)((kMaxTrain - passes) / 2);
        Train t;
        t.len = (uint8_t)(passes + 2 * prime);
        memset(t.code, kDarken, prime);
        memset(t.code + prime, kWhiten, passes + prime);
        return t;
    }

    // The passes a train has already run, undone: last first, polarity swapped.
    static Train undone(const Train& t, uint8_t elapsed) {
        Train r;
        r.len = elapsed;
        for (int i = 0; i < elapsed; ++i) {
            const uint8_t c = t.code[elapsed - 1 - i];
            r.code[i] = c == kDarken ? kWhiten : c == kWhiten ? kDarken : kFloat;
        }
        return r;
    }

    Cohort* claim(Kind kind) {
        for (Cohort& c : cohorts_) {
            if (c.kind != Kind::Free) continue;
            c.kind = kind;
            c.pass = 0;
            return &c;
        }
        return nullptr;
    }

    // Only the chunks it used, so retiring a small update costs a small update.
    static void clearMasked(uint8_t* plane, uint32_t* mask) {
        for (int row = 0; row < kHeight; ++row) {
            uint32_t m = mask[row];
            if (!m) continue;
            mask[row] = 0;
            for (int chunk = 0; chunk < kChunks; ++chunk) {
                if (m & chunkBit(chunk))
                    memset(plane + (size_t)row * kRowBytes + chunk * kChunkBytes, 0, kChunkBytes);
            }
        }
    }

    void release(Cohort& c) {
        clearMasked(c.plane, c.mask);
        c.kind = Kind::Free;
    }

    bool genDirty(const Gen& g) const {
        for (int row = 0; row < kHeight; ++row)
            if (g.mask[row]) return true;
        return false;
    }
    bool genDirty() const {
        for (const Gen& g : gens_)
            if (genDirty(g)) return true;
        return false;
    }

    static uint32_t* word(uint8_t* plane, size_t at) { return (uint32_t*)(plane + at); }

    bool submitWord(const uint8_t* frame, size_t at, int row, uint32_t bit, Cohort* fresh,
                    Cohort** reverseOf, Gen& cur) {
        const uint32_t levels = *(const uint32_t*)(frame + at);
        const uint32_t hi = levels & kSlotWhite;
        const uint32_t target = hi | (hi >> 1);        // 11 = black
        uint32_t* committed = word(committed_, at);
        uint32_t changed = target ^ *committed;
        if (!changed) return false;

        // A reversal and a settle are not cancelled: undoing a reversal would not
        // land anywhere useful, and neither runs long. Those pixels stay as they
        // are and go round again when the cohort retires.
        for (Cohort& c : cohorts_) {
            if (c.kind != Kind::Reverse && c.kind != Kind::Settle) continue;
            if (!(c.mask[row] & bit)) continue;
            const uint32_t locked = occupied(*word(c.plane, at)) & changed;
            if (!locked) continue;
            changed &= ~locked;
            deferred_[row] |= bit;
            anyDeferred_ = true;
            stats_.deferred++;
        }
        if (!changed) return false;

        bool work = false;
        for (int i = 0; i < kCohorts; ++i) {
            Cohort& c = cohorts_[i];
            if (c.kind != Kind::Fast || &c == fresh || !(c.mask[row] & bit)) continue;
            uint32_t* slots = word(c.plane, at);
            const uint32_t reversed = occupied(*slots) & changed;
            if (!reversed) continue;
            changed &= ~reversed;
            *committed ^= reversed;
            if (c.pass > 0) {
                if (!reverseOf[i]) {
                    Cohort* r = claim(Kind::Reverse);
                    r->toBlack = undone(c.toBlack, c.pass);
                    r->toWhite = undone(c.toWhite, c.pass);
                    r->len = c.pass;
                    reverseOf[i] = r;
                }
                *word(reverseOf[i]->plane, at) |= *slots & reversed;
                reverseOf[i]->mask[row] |= bit;
                work = true;
            }
            *slots &= ~reversed;
            mark(reversed, at, row, bit, cur);
            stats_.cancelled++;
        }

        if (changed) {
            *word(fresh->plane, at) = (changed & target & kSlotBlack) |
                                      (changed & ~target & kSlotWhite);
            fresh->mask[row] |= bit;
            *committed ^= changed;
            mark(changed, at, row, bit, cur);
            work = true;
        }
        return work;
    }

    // Everything in this chunk that is not moving, towards the rail it is on.
    void holdChunk(Cohort& hold, Cohort& fresh, size_t at, int row, uint32_t bit) {
        for (int w = 0; w < 4; ++w) {
            const size_t a = at + w * 4;
            const uint32_t still = ~occupied(*word(fresh.plane, a));
            const uint32_t black = *word(committed_, a);
            *word(hold.plane, a) = (still & black & kSlotBlack) | (still & ~black & kSlotWhite);
        }
        hold.mask[row] |= bit;
    }

    // `pixels` moved: start their stability clock over.
    void mark(uint32_t pixels, size_t at, int row, uint32_t bit, Gen& cur) {
        if (!params.settleAfterMs) return;
        for (Gen& g : gens_) {
            if (&g != &cur && (g.mask[row] & bit)) *word(g.plane, at) &= ~pixels;
        }
        *word(cur.plane, at) |= pixels;
        cur.mask[row] |= bit;
    }

    // White goes dark and back, black goes light and back: k of each, so net zero
    // and the pixel is driven hard towards the rail it should sit on last.
    void startSettle(Cohort& c, Gen& g) {
        const uint8_t k = params.settleK;
        c.toBlack = Train{};
        c.toWhite = Train{};
        c.toBlack.len = c.toWhite.len = (uint8_t)(2 * k);
        c.len = (uint8_t)(2 * k);
        for (int i = 0; i < k; ++i) {
            c.toBlack.code[i] = kWhiten;
            c.toBlack.code[k + i] = kDarken;
            c.toWhite.code[i] = kDarken;
            c.toWhite.code[k + i] = kWhiten;
        }
        for (int row = 0; row < kHeight; ++row) {
            const uint32_t m = g.mask[row];
            if (!m) continue;
            for (int chunk = 0; chunk < kChunks; ++chunk) {
                if (!(m & chunkBit(chunk))) continue;
                const size_t at = (size_t)row * kRowBytes + chunk * kChunkBytes;
                bool any = false;
                for (int w = 0; w < 4; ++w) {
                    const size_t a = at + w * 4;
                    const uint32_t due = *word(g.plane, a);
                    const uint32_t black = *word(committed_, a);
                    const uint32_t slots = (due & black & kSlotBlack) |
                                           (due & ~black & kSlotWhite);
                    *word(c.plane, a) = slots;
                    any |= slots != 0;
                }
                if (any) c.mask[row] |= chunkBit(chunk);
            }
        }
        clearMasked(g.plane, g.mask);
        // Masks are not narrowed when a pixel is struck out of a generation, so
        // this can come up empty: then there is nothing to drive.
        bool any = false;
        for (int row = 0; row < kHeight && !any; ++row) any = c.mask[row] != 0;
        if (!any) {
            c.kind = Kind::Free;
            return;
        }
        stats_.settles++;
    }

    void retryDeferred() {
        uint32_t again[kHeight];
        memcpy(again, deferred_, sizeof again);
        memset(deferred_, 0, sizeof deferred_);
        anyDeferred_ = false;
        if (submit(last_, again, 0)) return;
        memcpy(deferred_, again, sizeof again);
        anyDeferred_ = true;
    }

    Cohort cohorts_[kCohorts];
    Gen gens_[kGenerations];
    int genCur_ = 0;
    uint32_t genSince_ = 0;
    uint8_t* committed_ = nullptr;   // where each pixel will land, 11 = black
    uint8_t* last_ = nullptr;        // the newest frame, for going round again
    uint32_t deferred_[kHeight] = {};
    bool anyDeferred_ = false;
    Stats stats_;
};

}  // namespace hybrid
