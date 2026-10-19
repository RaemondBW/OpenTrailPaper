// Host tests for src/dash_layout.h — field registration, config parsing and
// serialization for the configurable dashboard.
//
// These catch the class of bug where a new field is added to the enum but its
// metadata entry lands in the wrong slot, or a config file that names it is
// silently rejected. The companion apps match by string id, so any mismatch
// between the enum position and the kFields array breaks the whole field.

#include <cstdio>
#include <cstring>
#include <string>

#include "dash_layout.h"
#include "stopwatch.h"
#include "ride_state.h"   // units:: — the speed conversion the page uses

#include <cmath>

namespace {

int g_fail = 0;
const char* g_case = "";

void check(bool ok, const std::string& what) {
    if (!ok) { printf("  FAIL  %s: %s\n", g_case, what.c_str()); ++g_fail; }
}

void checkStr(const char* got, const char* want, const std::string& what) {
    if (strcmp(got, want) != 0) {
        printf("  FAIL  %s: %s (got \"%s\", want \"%s\")\n",
               g_case, what.c_str(), got, want);
        ++g_fail;
    }
}

void begin(const char* name) { g_case = name; }

// --- Field registry ---------------------------------------------------------

// Every field in the enum must round-trip through its string id.
void testFieldIdRoundTrip() {
    begin("field registry: id round trip");
    for (uint8_t f = 0; f < DF_COUNT; ++f) {
        const char* id = dashFieldId(f);
        check(id[0] != '\0', std::string("field ") + std::to_string(f) + " has an id");
        uint8_t back = dashFieldFromId(id, strlen(id));
        check(back == f, std::string("dashFieldFromId(\"") + id + "\") == " +
                         std::to_string(f) + " (got " + std::to_string(back) + ")");
    }
}

// Every field has a non-empty label.
void testFieldLabels() {
    begin("field registry: labels");
    for (uint8_t f = 0; f < DF_COUNT; ++f) {
        const char* label = dashFieldLabel(f);
        check(label[0] != '\0', std::string("field ") + std::to_string(f) + " has a label");
    }
}

// The new avg speed field specifically.
void testAvgSpeedField() {
    begin("avg speed: registration");
    check(DF_AVG_SPEED < DF_COUNT, "DF_AVG_SPEED is in range");
    checkStr(dashFieldId(DF_AVG_SPEED), "avgspeed", "id is \"avgspeed\"");
    checkStr(dashFieldLabel(DF_AVG_SPEED), "AVG SPEED", "label is \"AVG SPEED\"");
    check(dashFieldFromId("avgspeed", 8) == DF_AVG_SPEED,
          "dashFieldFromId(\"avgspeed\") returns DF_AVG_SPEED");
}

// An unknown id still returns DF_COUNT.
void testUnknownField() {
    begin("field registry: unknown id");
    check(dashFieldFromId("nope", 4) == DF_COUNT, "unknown id returns DF_COUNT");
}

// --- Config parsing ---------------------------------------------------------

void testParseAvgSpeed() {
    begin("parse: avgspeed medium");
    DashLayout layout;
    bool ok = dashParse("avgspeed medium\n", layout);
    check(ok, "parse succeeds");
    check(layout.count == 1, "one item");
    check(layout.items[0].field == DF_AVG_SPEED, "field is DF_AVG_SPEED");
    check(layout.items[0].size == DZ_MEDIUM, "size is medium");
    check(!layout.items[0].half, "not half");
}

void testParseAvgSpeedHero() {
    begin("parse: avgspeed hero");
    DashLayout layout;
    dashParse("avgspeed hero\n", layout);
    check(layout.count == 1, "one item");
    check(layout.items[0].field == DF_AVG_SPEED, "field is DF_AVG_SPEED");
    check(layout.items[0].size == DZ_HERO, "size is hero");
}

void testParseAvgSpeedHalf() {
    begin("parse: avgspeed medium half");
    DashLayout layout;
    dashParse("avgspeed medium half\nspeed medium half\n", layout);
    check(layout.count == 2, "two items");
    check(layout.items[0].field == DF_AVG_SPEED, "first field is DF_AVG_SPEED");
    check(layout.items[0].half, "first is half");
    check(layout.items[1].field == DF_SPEED, "second field is DF_SPEED");
}

// --- Serialization ----------------------------------------------------------

void testSerializeRoundTrip() {
    begin("serialize: avgspeed round trips");
    DashLayout layout;
    dashParse("avgspeed large\nspeed medium half\ndistance medium half\n", layout);
    check(layout.count == 3, "three items parsed");

    char buf[512];
    size_t n = dashSerialize(layout, buf, sizeof(buf));
    check(n > 0, "serialized");

    DashLayout back;
    bool ok = dashParse(buf, back);
    check(ok, "re-parse succeeds");
    check(back.count == layout.count, "same count after round trip");
    check(back.items[0].field == DF_AVG_SPEED, "first field survives round trip");
    check(back.items[0].size == DZ_LARGE, "size survives round trip");
}

// --- Pages ------------------------------------------------------------------

void testPagesWithAvgSpeed() {
    begin("pages: avgspeed in layout and map strip");
    const char* cfg =
        "avgspeed hero\n"
        "distance medium half\n"
        "ridetime medium half\n"
        "map avgspeed distance ridetime\n"
        "page map\n";
    DashPages pages;
    bool ok = dashParsePages(cfg, pages);
    check(ok, "parse succeeds");
    // First page is fields.
    check(pages.pages[0].kind == DP_FIELDS, "first page is fields");
    check(pages.pages[0].layout.items[0].field == DF_AVG_SPEED,
          "first field is DF_AVG_SPEED");
    // Map strip.
    check(pages.mapFields[0] == DF_AVG_SPEED, "map strip field 0 is avgspeed");
    check(pages.mapFields[1] == DF_DISTANCE, "map strip field 1 is distance");
    check(pages.mapFields[2] == DF_RIDE_TIME, "map strip field 2 is ridetime");
}

void testSerializePagesRoundTrip() {
    begin("pages: avgspeed serialization round trip");
    const char* cfg =
        "avgspeed hero\n"
        "hr medium half\n"
        "cadence medium half\n"
        "map avgspeed distance ridetime\n"
        "page map\n";
    DashPages pages;
    dashParsePages(cfg, pages);

    char buf[1024];
    size_t n = dashSerializePages(pages, buf, sizeof(buf));
    check(n > 0, "serialized");

    DashPages back;
    bool ok = dashParsePages(buf, back);
    check(ok, "re-parse succeeds");
    check(back.pages[0].layout.items[0].field == DF_AVG_SPEED,
          "avgspeed survives page round trip");
    check(back.mapFields[0] == DF_AVG_SPEED, "map strip survives round trip");
}

// --- Stopwatch page ---------------------------------------------------------

void testStopwatchPage() {
    begin("pages: stopwatch parses and round trips");
    const char* cfg =
        "speed hero\n"
        "page stopwatch\n"
        "page map\n";
    DashPages pages;
    bool ok = dashParsePages(cfg, pages);
    check(ok, "parse succeeds");
    check(pages.count == 3, "fields + stopwatch + map");
    check(pages.pages[1].kind == DP_STOPWATCH, "second page is the stopwatch");
    check(pages.pages[1].layout.count == 0, "stopwatch carries no items");

    char buf[1024];
    size_t n = dashSerializePages(pages, buf, sizeof(buf));
    check(n > 0, "serialized");
    check(strstr(buf, "page stopwatch\n") != nullptr,
          "serializes as `page stopwatch`");
    DashPages back;
    check(dashParsePages(buf, back), "re-parse succeeds");
    check(back.count == 3 && back.pages[1].kind == DP_STOPWATCH,
          "stopwatch survives round trip in place");
}

// --- Segment page -----------------------------------------------------------

void testSegmentPage() {
    begin("pages: segment parses and round trips, with its manual option");
    const char* cfg =
        "speed hero\n"
        "page segment\n"
        "page map\n";
    DashPages pages;
    check(dashParsePages(cfg, pages), "parse succeeds");
    check(pages.count == 3 && pages.pages[1].kind == DP_SEGMENT,
          "second page is the segment page");
    check(pages.pages[1].autoShow, "switches to itself by default");
    char buf[1024];
    check(dashSerializePages(pages, buf, sizeof(buf)) > 0, "serialized");
    check(strstr(buf, "page segment\n") != nullptr, "serializes as `page segment`");

    DashPages manual;
    check(dashParsePages("speed hero\npage segment manual\n", manual),
          "manual parses");
    check(manual.pages[1].kind == DP_SEGMENT && !manual.pages[1].autoShow,
          "manual = no auto-switch");
    check(dashSerializePages(manual, buf, sizeof(buf)) > 0, "serialized");
    check(strstr(buf, "page segment manual\n") != nullptr, "manual round trips");
    DashPages back;
    check(dashParsePages(buf, back) && back.pages[1].kind == DP_SEGMENT &&
              !back.pages[1].autoShow,
          "manual survives a round trip");
}

// Older firmware sees `page stopwatch` as an unknown kind: an empty field page,
// dropped. This parser applies the same rule to any FUTURE kind, so a newer
// app's config never breaks the layout around it.
void testUnknownPageKindDropped() {
    begin("pages: unknown page kind is dropped, rest kept");
    const char* cfg =
        "speed hero\n"
        "page someday\n"
        "page\n"
        "hr medium\n";
    DashPages pages;
    check(dashParsePages(cfg, pages), "parse succeeds");
    check(pages.count == 3, "two field pages + appended map");
    check(pages.pages[0].kind == DP_FIELDS && pages.pages[1].kind == DP_FIELDS,
          "unknown page vanished, field pages kept");
    check(pages.pages[2].kind == DP_MAP, "map appended");
}

void testStopwatchClock() {
    begin("stopwatch: start / pause / resume / reset");
    Stopwatch sw;
    check(sw.zero() && sw.elapsedMs(5000) == 0, "starts at zero");
    sw.toggle(1000);
    check(sw.running, "running after start");
    check(sw.elapsedMs(4500) == 3500, "counts while running");
    check(!sw.reset(), "reset refused while running");
    check(sw.elapsedMs(4500) == 3500, "refused reset kept the time");
    sw.toggle(6000);
    check(!sw.running && sw.elapsedMs(60000) == 5000, "stop banks and freezes");
    sw.toggle(10000);
    check(sw.elapsedMs(12000) == 7000, "resume continues from the banked time");
    sw.toggle(12000);
    check(sw.reset() && sw.zero(), "reset when stopped");

    begin("stopwatch: laps");
    Stopwatch l;
    check(!l.lap(0), "no lap before start");
    l.toggle(0);
    check(l.lap(60000) && l.lap(125000) && l.lap(170500), "three laps");
    check(l.lapCount == 3, "lap count");
    check(l.lapSplitMs(0) == 60000 && l.lapSplitMs(1) == 65000 &&
              l.lapSplitMs(2) == 45500,
          "splits are the differences between marks");
    check(l.lapEndMs[2] == 170500, "cumulative time kept per lap");
    check(l.currentLapMs(180000) == 9500, "lap in progress since last mark");
    l.toggle(200000);
    check(!l.lap(210000), "no lap while paused");
    check(l.currentLapMs(999999) == 29500, "paused lap is frozen");
    l.toggle(300000);
    check(l.lap(310000) && l.lapEndMs[3] == 210000,
          "lap after resume counts only running time");
    check(!l.reset() && l.lapCount == 4, "running reset keeps laps");
    l.toggle(320000);
    check(l.reset() && l.lapCount == 0 && l.zero(), "reset clears laps");

    begin("stopwatch: lap limit");
    Stopwatch full;
    full.toggle(0);
    for (int i = 1; i <= STOPWATCH_MAX_LAPS; ++i) full.lap((uint32_t)i * 1000);
    check(full.lapsFull() && full.lapCount == STOPWATCH_MAX_LAPS, "fills to max");
    check(!full.lap(500000) && full.lapEndMs[0] == 1000,
          "a lap past the max is refused, lap 1 kept");
    check(!full.canLap(), "LAP disabled at the cap");

    // The three footer buttons: START/PAUSE always live; RESET only paused
    // with something to clear; LAP only running and under the cap.
    begin("stopwatch: button enablement");
    Stopwatch b;
    check(!b.canReset() && !b.canLap(), "ready: RESET and LAP disabled");
    b.toggle(0);
    check(!b.canReset() && b.canLap(), "running: LAP live, RESET disabled");
    b.toggle(5000);
    check(b.canReset() && !b.canLap(), "paused: RESET live, LAP disabled");
    b.reset();
    check(!b.canReset() && !b.canLap(), "after reset: back to ready");
    b.toggle(0);
    b.toggle(0);   // started and paused at once: nothing to clear
    check(!b.canReset(), "paused at zero: RESET disabled");

    begin("stopwatch: lap averages");
    {
        Stopwatch a;
        a.sample(0, 30.0f, true, 200, true);   // before start: not counted
        a.toggle(10000);
        check(a.currentAvgSpeedKmh() < 0 && a.currentAvgPowerW() < 0,
              "nothing integrated before the first running interval");
        // 60 s at 30 km/h / 200 W, then 60 s at 20 km/h / 0 W (coasting).
        a.sample(70000, 20.0f, true, 0, true);
        a.sample(130000, 20.0f, true, 0, true);
        check(std::fabs(a.currentAvgSpeedKmh() - 25.0f) < 0.01f,
              "speed is distance over time (25 km/h)");
        check(a.currentAvgPowerW() == 100, "zeros count in the power mean");
        check(a.lap(130000), "lap");
        check(a.lapSpeedDkmh[0] == 250 && a.lapAvgPowerW(0) == 100,
              "lap stores its averages");
        check(std::fabs(a.lapAvgSpeedKmh(0) - 25.0f) < 0.01f, "speed readback");
        check(a.currentAvgSpeedKmh() < 0, "new lap starts empty");

        // Stop mid-lap: the stopped minute adds nothing.
        a.sample(160000, 40.0f, true, 300, true);   // 30 s at 20 / 0
        a.toggle(190000);                            // 30 s at 40 / 300, stop
        a.sample(220000, 99.0f, true, 999, true);    // stopped: ignored
        a.sample(250000, 99.0f, true, 999, true);
        a.toggle(250000);                            // resume, held 99/999
        a.sample(250000, 40.0f, true, 300, true);    // replaced at once
        a.sample(310000, 40.0f, true, 300, true);    // 60 s at 40 / 300
        // Running time only: 30 s @20, 30 s @40, 60 s @40 ->
        // (20*30 + 40*30 + 40*60) / 120 = 35 km/h; power (0*30+300*90)/120.
        check(std::fabs(a.currentAvgSpeedKmh() - 35.0f) < 0.01f,
              "stopped time excluded from speed");
        check(a.currentAvgPowerW() == 225, "stopped time excluded from power");

        // No meter: dash. Meter dropping out mid-lap: only its time counts.
        Stopwatch b;
        b.toggle(0);
        b.sample(0, 25.0f, true, 0, false);
        b.sample(60000, 25.0f, true, 0, false);
        b.lap(60000);
        check(b.lapAvgPowerW(0) == -1 && b.lapPowerW[0] == STOPWATCH_NO_VALUE,
              "no power meter -> no value");
        b.sample(60000, 25.0f, true, 250, true);
        b.sample(90000, 25.0f, true, 0, false);   // meter silent from here
        b.sample(150000, 25.0f, true, 0, false);
        check(b.currentAvgPowerW() == 250, "silent meter time not counted");

        // No speed source for the whole lap: dash, not 0.
        Stopwatch c;
        c.toggle(0);
        c.sample(0, 0.0f, false, 180, true);
        c.lap(45000);
        check(c.lapAvgSpeedKmh(0) < 0 && c.lapAvgPowerW(0) == 180,
              "no speed source -> no speed, power still there");

        // Lap boundary between samples is credited exactly.
        Stopwatch d;
        d.toggle(0);
        d.sample(0, 36.0f, true, 100, true);
        d.lap(500);                                // mid-hold
        d.sample(1000, 18.0f, true, 300, true);
        check(d.lapSpeedDkmh[0] == 360 && d.lapAvgPowerW(0) == 100,
              "first lap holds its own sample to the boundary");
        check(std::fabs(d.currentAvgSpeedKmh() - 36.0f) < 0.01f,
              "the rest of that hold lands in the next lap");

        // Reset clears the averages with the laps.
        a.toggle(310000);
        check(a.reset() && a.currentAvgSpeedKmh() < 0 &&
                  a.currentAvgPowerW() < 0,
              "reset clears lap averages");
    }

    begin("stopwatch: cadence averages");
    {
        Stopwatch c;
        c.toggle(0);
        // 60 s at 90 rpm, 30 s coasting at 0, 30 s at 80, then a pause.
        c.sample(0, 30.0f, true, 0, false, 90, true);
        c.sample(60000, 30.0f, true, 0, false, 0, true);
        c.sample(90000, 30.0f, true, 0, false, 80, true);
        c.toggle(120000);
        c.sample(150000, 30.0f, true, 0, false, 200, true);   // paused: ignored
        c.toggle(180000);
        c.sample(180000, 30.0f, true, 0, false, 80, true);
        c.sample(240000, 30.0f, true, 0, false, 80, true);    // 60 s at 80
        // Pedalling: 60 s @90 + 90 s @80 = (5400 + 7200) / 150 = 84 rpm.
        check(c.currentAvgCadenceRpm() == 84,
              "zeros and paused time excluded from cadence");
        check(c.lap(240000) && c.lapAvgCadenceRpm(0) == 84, "lap stores it");
        // Coasting the whole lap: a source, but no average.
        c.sample(240000, 30.0f, true, 0, false, 0, true);
        c.lap(300000);
        check(c.lapAvgCadenceRpm(1) == -1 &&
                  c.lapCadenceRpm[1] == STOPWATCH_NO_CADENCE,
              "all-zero lap -> no cadence value");
        // No source at all.
        c.sample(300000, 30.0f, true, 0, false, 0, false);
        c.lap(360000);
        check(c.lapAvgCadenceRpm(2) == -1, "no cadence source -> no value");
        c.toggle(360000);
        check(c.reset() && c.currentAvgCadenceRpm() < 0, "reset clears cadence");
    }

    begin("stopwatch: which metric columns show");
    {
        // None: no meter, no cadence, ever.
        Stopwatch n;
        n.toggle(0);
        n.sample(0, 25.0f, true, 0, false, 0, false);
        n.sample(60000, 25.0f, true, 0, false, 0, false);
        n.lap(60000);
        check(!n.showPower() && !n.showCadence(), "none");

        // Power only (a meter reporting, no cadence).
        Stopwatch p;
        p.sample(0, 0.0f, false, 0, true, 0, false);
        check(p.showPower() && !p.showCadence(),
              "power only — even before START, the meter is reporting");

        // Cadence only.
        Stopwatch c;
        c.sample(0, 0.0f, false, 0, false, 85, true);
        check(!c.showPower() && c.showCadence(), "cadence only");

        // Both.
        Stopwatch b;
        b.sample(0, 0.0f, false, 200, true, 85, true);
        check(b.showPower() && b.showCadence(), "both");

        // A meter appearing mid-session: column appears; stays once a lap
        // has its data, even after the meter goes quiet.
        Stopwatch m;
        m.toggle(0);
        m.sample(0, 25.0f, true, 0, false);
        m.sample(30000, 25.0f, true, 0, false);
        check(!m.showPower(), "no meter yet: no WATTS");
        m.sample(60000, 25.0f, true, 220, true);
        check(m.showPower(), "meter appears: WATTS");
        m.sample(90000, 25.0f, true, 0, false);       // meter goes quiet
        check(m.showPower(), "quiet meter, current lap has data: kept");
        m.lap(120000);
        m.sample(120000, 25.0f, true, 0, false);
        m.lap(180000);
        check(m.showPower() && m.lapAvgPowerW(1) == -1,
              "laps had power: kept, the quiet lap reads --");
        m.toggle(180000);
        m.reset();
        check(!m.showPower(), "reset with no meter: WATTS goes");
    }

    begin("stopwatch: speed units");
    check(std::fabs(units::speed(25.0, true) - 15.534) < 0.01,
          "25 km/h shows as 15.5 mph");
    check(std::fabs(units::speed(25.0, false) - 25.0) < 1e-9,
          "km/h unchanged");

    begin("stopwatch: millis() wrap");
    Stopwatch w;
    w.toggle(0xFFFFFC18u);   // 1 s before the wrap
    check(w.elapsedMs(1000) == 2000, "elapsed is right across the wrap");
}

}  // namespace

int main() {
    printf("dashboard layout tests\n");
    testFieldIdRoundTrip();
    testFieldLabels();
    testAvgSpeedField();
    testUnknownField();
    testParseAvgSpeed();
    testParseAvgSpeedHero();
    testParseAvgSpeedHalf();
    testSerializeRoundTrip();
    testPagesWithAvgSpeed();
    testSerializePagesRoundTrip();
    testStopwatchPage();
    testSegmentPage();
    testUnknownPageKindDropped();
    testStopwatchClock();

    if (g_fail == 0) { printf("all dash layout tests passed\n"); return 0; }
    printf("%d dash layout assertion(s) FAILED\n", g_fail);
    return 1;
}
