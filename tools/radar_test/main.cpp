#include <initializer_list>
#include "radar.h"
#include "dash_layout.h"
#include <cassert>
#include <cstring>
#include <cstdio>

int main() {
    RadarState s;
    s.connected = true;
    const uint8_t two[] = {0x02, 0x81, 118, 34, 0x82, 28, 1};
    assert(radarIngest(s, two, sizeof(two), 100));
    assert(s.live && s.count == 2 && s.targets[0].distanceM == 28);
    // The byte after distance is not interpreted as speed.
    const uint8_t frag[] = {0x04, 0x83, 70, 255};
    assert(radarIngest(s, frag, sizeof(frag), 110) && s.count == 3);
    const uint8_t malformed[] = {0x12, 0x84};
    assert(!radarIngest(s, malformed, sizeof(malformed), 120));
    const uint8_t sector[] = {0x06, 0x30, 0x05, 0, 0, 70};
    assert(!radarIngest(s, sector, sizeof(sector), 120));
    assert(!radarIngest(s, nullptr, 0, 120));
    assert(s.packetMs == 110 && s.count == 3);
    // Placeholders, status marker and uncertain range must not create cars.
    const uint8_t sentinels[] = {0x12, 0, 50, 0, 0xfd, 0, 0, 0x84, 255, 0};
    assert(radarIngest(s, sentinels, sizeof(sentinels), 130) && s.count == 3);
    // Heartbeats do not erase other fragments, but old tracks age out.
    const uint8_t beat[] = {0x22};
    assert(radarIngest(s, beat, 1, 200) && s.count == 3);
    assert(radarIngest(s, beat, 1, 2200) && s.count == 0 && s.live);
    radarExpire(s, 5201);
    assert(!s.live && s.count == 0);
    assert(radarIngest(s, two, sizeof(two), 6000) && s.count == 2 && s.live);
    const uint8_t passed[] = {0x32, 0x82, 0, 0};
    assert(radarIngest(s, passed, sizeof(passed), 6010) && s.count == 1);
    s.connected = false;
    radarExpire(s, 6020);
    assert(!s.live && s.count == 0);
    // Unsigned age calculations survive millis() wrap.
    s = RadarState{}; s.connected = true;
    assert(radarIngest(s, two, sizeof(two), 0xfffffff0U));
    radarExpire(s, 50);
    assert(s.live && s.count == 2);
    radarExpire(s, 4000);
    assert(!s.live && s.count == 0);
    // Bounds and nearest-first ordering with more targets over several frames.
    s = RadarState{}; s.connected = true;
    for (int i = 0; i < 20; ++i) {
        uint8_t frame[] = {0x02, uint8_t(0x80 | i), uint8_t(140 - i), 0};
        assert(radarIngest(s, frame, sizeof(frame), 100 + i));
        assert(s.count <= RADAR_MAX_TARGETS);
    }
    assert(s.count == 8 && s.targets[0].distanceM == 121 && s.targets[7].distanceM == 128);
    // Existing configs keep their meaning; vertical normalizes consistently.
    DashPages pages;
    assert(dashParsePages("speed large\nhr medium half\ncadence medium half\nradar medium vertical\npage map\n", pages));
    assert(pages.count == 2 && pages.pages[0].layout.count == 4);
    assert(pages.pages[0].layout.items[3].vertical && !pages.pages[0].layout.items[3].half);
    char config[2048];
    assert(dashSerializePages(pages, config, sizeof(config)));
    DashPages again;
    assert(dashParsePages(config, again) && again.pages[0].layout.items[3].field == DF_RADAR);
    DashLayout bad;
    assert(dashParse("speed small vertical\npower medium vertical\nradar medium\n", bad));
    assert(bad.count == 3 && !bad.items[0].vertical && !bad.items[1].vertical && bad.items[2].vertical);
    assert(dashParse("radar medium height=50\nradar medium height=75\n", bad));
    assert(bad.count == 1 && bad.items[0].heightPercent == 50);
    assert(dashParsePages("map radar hr clock\nspeed hero\n", pages));
    assert(pages.mapFields[0] == DF_SPEED && pages.mapFields[1] == DF_HEART_RATE);
    // Height survives both wire formats; old/invalid configs keep full height.
    for (int height : {50, 75, 100}) {
        char input[160];
        snprintf(input, sizeof(input), "speed large\nradar medium half vertical height=%d\n", height);
        DashLayout sized;
        assert(dashParse(input, sized) && sized.items[1].heightPercent == height);
        assert(!sized.items[1].half);
        assert(dashSerialize(sized, config, sizeof(config)));
        assert(dashParse(config, bad) && bad.items[1].heightPercent == height);
        assert(dashParsePages(input, pages));
        assert(dashSerializePages(pages, config, sizeof(config)));
        assert(dashParsePages(config, again) && again.pages[0].layout.items[1].heightPercent == height);
    }
    assert(dashParse("radar medium vertical height=12\n", bad) && bad.items[0].heightPercent == 100);
    assert(dashParse("radar medium\n", bad) && bad.items[0].heightPercent == 100);
    assert(dashParse("speed small height=50\nhr medium vertical height=75\npower medium vertical height=50\n", bad));
    assert(bad.items[0].heightPercent == 100 && bad.items[1].heightPercent == 100 && bad.items[2].heightPercent == 100);
    // Actual weighted grid used by the preview; include navigation and a
    // requested edge in the gutter between two equal rows.
    const int grid[] = {257, 192, 192, 183};
    const int navGrid[] = {214, 161, 161, 150};
    const int equalGrid[] = {424, 424};
    const int single[] = {860};
    assert(dashVerticalHeight(grid, 4, 12, 50) == 461);
    assert(dashVerticalHeight(grid, 4, 12, 75) == 665);
    assert(dashVerticalHeight(grid, 4, 12, 100) == 860);
    assert(dashVerticalHeight(navGrid, 4, 12, 50) == 387);
    assert(dashVerticalHeight(navGrid, 4, 12, 75) == 560);
    assert(dashVerticalHeight(navGrid, 4, 12, 100) == 722);
    assert(dashVerticalHeight(equalGrid, 2, 12, 50) == 424);
    assert(dashVerticalHeight(single, 1, 12, 50) == 860);
    assert(dashVerticalHeight(nullptr, 0, 12, 50) == 0);
    const int heroGrid[] = {484, 181, 171};
    assert(dashVerticalHeight(heroGrid, 3, 12, 50, true) == 364);
    assert(dashVerticalHeight(grid, 4, 12, 50, true) == 387);
    assert(dashVerticalHeight(navGrid, 4, 12, 50, true) == 323);
    assert(dashVerticalHeight(equalGrid, 2, 12, 50, true) == 424);
    assert(dashVerticalHeight(grid, 4, 12, 100, true) == 860);
    assert(dashParse("radar medium half vertical height=50 position=bottom\n", bad));
    assert(bad.items[0].bottomAligned && bad.items[0].heightPercent == 50);
    assert(dashSerialize(bad, config, sizeof(config)));
    assert(dashParse(config, bad) && bad.items[0].bottomAligned);
    assert(dashParsePages(config, pages));
    assert(dashSerializePages(pages, config, sizeof(config)));
    assert(dashParsePages(config, again) && again.pages[0].layout.items[0].bottomAligned);
    assert(dashParse("radar medium position=top\n", bad) && !bad.items[0].bottomAligned);
    assert(dashParse("power hero position=bottom\n", bad) && !bad.items[0].bottomAligned);
    puts("Radar protocol and layout tests passed");
}
