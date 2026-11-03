// What counts as the same map view, and what the cache does when it fills up.
#include <stdio.h>

#include "../../src/map_base_cache.h"

namespace {

int g_failures = 0;

void expect(bool ok, const char* name) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) g_failures++;
}

MapBaseKey view(double lat, double lon, float mpp, float rot = 0) {
    MapBaseKey k;
    k.lat = lat;
    k.lon = lon;
    k.mpp = mpp;
    k.rotDeg = rot;
    k.layers = 3;
    k.hasMap = true;
    return k;
}

constexpr double kMetre = 1.0 / 111320.0;   // degrees of latitude

void sameView() {
    const MapBaseKey a = view(37.77, -122.43, 2);
    expect(mapSameView(a, a), "identical views match");
    expect(mapSameView(a, view(37.77 + 3 * kMetre, -122.43, 2)), "GPS wander under 2 px matches");
    expect(!mapSameView(a, view(37.77 + 6 * kMetre, -122.43, 2)), "more than 2 px does not");
    expect(mapSameView(view(37.77, -122.43, 32), view(37.77 + 50 * kMetre, -122.43, 32)),
           "the same wander allowance scales with zoom");
    expect(!mapSameView(a, view(37.77, -122.43, 4)), "another zoom never matches");
    expect(mapSameView(a, view(37.77, -122.43, 2, 1)), "a small turn matches");
    expect(!mapSameView(a, view(37.77, -122.43, 2, 3)), "a bigger turn does not");
    expect(mapSameView(view(37.77, -122.43, 2, 359.5f), view(37.77, -122.43, 2, 0.5f)),
           "turns wrap at north");
    MapBaseKey layers = a;
    layers.layers = 1;
    expect(!mapSameView(a, layers), "different layers never match");
    MapBaseKey newTiles = a;
    newTiles.tiles = a.tiles + 1;
    expect(!mapSameView(a, newTiles), "a tile changing on the card never matches");
    MapBaseKey noMap = a;
    noMap.hasMap = false;
    expect(!mapSameView(a, noMap), "map coverage has to match");
    // A degree of longitude is shorter away from the equator.
    expect(mapSameView(view(69, 20, 2), view(69, 20 + 8 * kMetre, 2)),
           "east-west distance allows for latitude");
}

void cache() {
    static uint8_t px[MapBaseCache::kSlots][8];
    MapBaseCache c;
    expect(c.claim(0) == nullptr, "no pixels, nothing to claim");
    for (int i = 0; i < MapBaseCache::kSlots; ++i) c.attach(i, px[i]);

    const MapBaseKey a = view(37.77, -122.43, 2), b = view(37.77, -122.43, 4),
                     d = view(37.77, -122.43, 8), f = view(37.77, -122.43, 32);
    uint8_t* slot = c.claim(100);
    expect(c.find(a, 100) == nullptr, "a claimed layer is not findable before commit");
    c.commit(slot, a, 100);
    expect(c.find(a, 150) == slot, "a committed layer is found");

    uint8_t* sb = c.claim(200); c.commit(sb, b, 200);
    uint8_t* sd = c.claim(300); c.commit(sd, d, 300);
    c.find(a, 500);             // a is now the most recently used
    uint8_t* sf = c.claim(600);
    expect(sf == sb, "full: the longest unused layer goes");
    expect(c.find(b, 600) == nullptr, "and can no longer be found");
    expect(c.find(a, 600) == slot, "while the one in use stays");
    c.commit(sf, f, 600);

    expect(c.find(a, 100 + 9 * 60 * 1000) == slot, "still good after 9 minutes");
    expect(c.find(a, 300 + 11 * 60 * 1000) == nullptr, "gone after 10");
    expect(c.claim(1000) == slot, "and its slot is free again");

    c.clear();
    expect(c.find(f, 1000) == nullptr, "clear empties it");
}

void clockWrap() {
    static uint8_t px[8];
    MapBaseCache c;
    c.attach(0, px);
    const MapBaseKey a = view(37.77, -122.43, 2);
    const uint32_t nearWrap = 0xFFFFFF00u;
    c.commit(c.claim(nearWrap), a, nearWrap);
    expect(c.find(a, nearWrap + 0x200) == px, "survives millis() wrapping");
}

}  // namespace

int main() {
    sameView();
    cache();
    clockWrap();
    printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
