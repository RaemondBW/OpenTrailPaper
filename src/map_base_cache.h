// Finished map layers, kept so a frame does not have to render one again.
//
// The base layer (fills and roads) is the slow part of a map frame by a long way, and
// nothing on it moves unless the view does: the route, POIs, rider and buttons all go
// on top afterwards. So it is worth keeping the layer for the view on screen, which the
// 1 Hz redraw would otherwise rebuild every time, and worth rendering the zooms either
// side while the bike is still, which is what makes a zoom tap land at once.
//
// Hardware-free apart from the pixels, which the caller allocates, so tools/map_cache_test
// can run it on the host.
#pragma once

#include <math.h>
#include <stdint.h>

struct MapBaseKey {
    double lat = 0, lon = 0;
    float mpp = 0;
    float rotDeg = 0;
    uint32_t layers = 0;
    uint32_t tiles = 0;     // map_store::generation(): the tiles it was drawn from
    bool hasMap = false;
};

// Whether a layer drawn for `a` will do for `b`. Zoom and content must match exactly,
// so a tile changing on the card retires every layer drawn before it.
// Position and rotation only need to be close: a stationary GPS fix wanders, a compass
// more so, and an exact match would mean the cache never hit while standing still.
inline bool mapSameView(const MapBaseKey& a, const MapBaseKey& b) {
    constexpr double kMaxShiftPx = 2.0;
    constexpr float kMaxTurnDeg = 1.5f;
    constexpr double kMetresPerDeg = 111320.0;
    if (a.mpp != b.mpp || a.layers != b.layers || a.tiles != b.tiles ||
        a.hasMap != b.hasMap) return false;
    const double north = (a.lat - b.lat) * kMetresPerDeg;
    const double east = (a.lon - b.lon) * kMetresPerDeg * cos(a.lat * M_PI / 180.0);
    const double limit = kMaxShiftPx * a.mpp;
    if (north * north + east * east > limit * limit) return false;
    float turn = fmodf(fabsf(a.rotDeg - b.rotDeg), 360.0f);
    if (turn > 180.0f) turn = 360.0f - turn;
    return turn <= kMaxTurnDeg;
}

class MapBaseCache {
public:
    // The view on screen and one zoom either side.
    static constexpr int kSlots = 3;

    void attach(int slot, uint8_t* pixels) { slots_[slot].pixels = pixels; }

    uint8_t* find(const MapBaseKey& key, uint32_t nowMs) {
        for (Slot& s : slots_) {
            if (!s.filled || !mapSameView(s.key, key)) continue;
            if (nowMs - s.storedMs > kMaxAgeMs) {
                s.filled = false;
                continue;
            }
            s.usedMs = nowMs;
            return s.pixels;
        }
        return nullptr;
    }

    // Somewhere to write a layer for `key`: an empty slot, else the longest unused. It
    // holds nothing until commit(), so an abandoned write can never be found.
    uint8_t* claim(uint32_t nowMs) {
        Slot* pick = nullptr;
        for (Slot& s : slots_) {
            if (!s.pixels) continue;
            if (!s.filled) {
                pick = &s;
                break;
            }
            if (!pick || nowMs - s.usedMs > nowMs - pick->usedMs) pick = &s;
        }
        if (!pick) return nullptr;
        pick->filled = false;
        return pick->pixels;
    }

    void commit(uint8_t* pixels, const MapBaseKey& key, uint32_t nowMs) {
        for (Slot& s : slots_) {
            if (s.pixels != pixels) continue;
            s.key = key;
            s.storedMs = s.usedMs = nowMs;
            s.filled = true;
        }
    }

    void clear() {
        for (Slot& s : slots_) s.filled = false;
    }

private:
    // Not for staleness, which the key covers. A layer outlives whatever made it useful,
    // and this is what stops it being kept going by a view nobody returns to.
    static constexpr uint32_t kMaxAgeMs = 10 * 60 * 1000;

    struct Slot {
        MapBaseKey key;
        uint8_t* pixels = nullptr;
        uint32_t storedMs = 0, usedMs = 0;
        bool filled = false;
    };
    Slot slots_[kSlots];
};
