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

    if (g_fail == 0) { printf("all dash layout tests passed\n"); return 0; }
    printf("%d dash layout assertion(s) FAILED\n", g_fail);
    return 1;
}
