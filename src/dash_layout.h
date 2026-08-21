#pragma once

#include <stdint.h>
#include <stddef.h>

// Rider-configurable dashboard layout.
//
// The dashboard used to be a hardcoded hero cell over a fixed 2x2 grid. This
// describes it as DATA instead: an ordered list of fields, each with a size and
// a width, which the renderer packs top to bottom. Riders care about different
// numbers — a commuter wants distance and clock, a racer wants power and heart
// rate — and the panel is the one part of the device they cannot rearrange.
//
// HOST-SAFE. This header and dash_layout.cpp are compiled by the preview tool
// alongside ui_render.cpp (tools/preview/render_preview.sh), so nothing here may
// touch Arduino, SD or NVS. Loading and saving the config FILE lives in
// dash_config.* on the firmware side; this half only knows how to parse the
// text, hold the result, and describe the fields.

// Every value the device can put in a cell. Each maps to something already
// measured — a sensor, the recorder, or the GPS — so adding a field here means
// wiring it in dashFieldValue() (ui_render.cpp), not inventing new plumbing.
enum DashField : uint8_t {
    DF_SPEED = 0,
    DF_POWER3S,      // 3 s rolling average — the one worth riding to
    DF_POWER,        // instantaneous
    DF_HEART_RATE,
    DF_CADENCE,
    DF_DISTANCE,
    DF_RIDE_TIME,    // elapsed since start
    DF_MOVING_TIME,  // elapsed minus stops
    DF_CLIMB,        // cumulative ascent, from the map DEM
    DF_GRADE,
    DF_ALTITUDE,     // map DEM elevation, not the GPS chip's
    DF_BATTERY,
    DF_SATELLITES,
    DF_CLOCK,
    DF_ROUTE_LEFT,   // distance remaining on the loaded route
    DF_COUNT
};

// Cell size. This is a HEIGHT WEIGHT, not a font: the renderer divides the
// available height between rows in proportion to these, then picks the largest
// font that fits the row it ends up with. So a layout of four SMALL fields
// still fills the panel — size controls the share, not an absolute pixel count,
// which is what keeps any combination of fields looking deliberate.
enum DashSize : uint8_t {
    DZ_SMALL = 0,
    DZ_MEDIUM,
    DZ_LARGE,
    DZ_HERO,         // the huge Impact_128 number, ~a third of the panel
    DZ_COUNT
};

struct DashItem {
    uint8_t field = DF_SPEED;   // DashField
    uint8_t size = DZ_MEDIUM;   // DashSize
    bool    half = false;       // share its row with the next half field
};

// 12 is well past what fits legibly on a 540x960 panel (the default uses 5) and
// keeps the whole layout a fixed-size POD, so it can live in a struct that is
// copied around and parsed without allocation.
constexpr int DASH_MAX_ITEMS = 12;

struct DashLayout {
    DashItem items[DASH_MAX_ITEMS];
    int count = 0;
};

// --- Multiple pages ---------------------------------------------------------
// The dashboard is a short carousel of pages the Home key steps through. Each
// page is either a field layout or the MUSIC page (phone media controls +
// album art), which carries no items — its content comes over BLE.
//
// The MAP is itself a page in the cycle (`page map`), movable like the rest —
// there is always exactly one, the parser appends it when a config predates
// its existence, and it can't be removed. 5 = 4 configurable pages + the map.
constexpr int DASH_MAX_PAGES = 5;

enum DashPageKind : uint8_t {
    DP_FIELDS = 0,
    DP_MUSIC,
    DP_MAP,
};

struct DashPage {
    uint8_t kind = DP_FIELDS;   // DashPageKind
    DashLayout layout;          // empty when kind == DP_MUSIC
};

struct DashPages {
    DashPage pages[DASH_MAX_PAGES];
    int count = 0;
    // The map screen's 3-cell data strip, configured by a `map <f> <f> <f>`
    // line in the same file. Not a page — the map is always in the carousel —
    // but its fields ride along in the one config.
    uint8_t mapFields[3] = {DF_SPEED, DF_DISTANCE, DF_RIDE_TIME};
};

// The built-in layout: today's dashboard, field for field, so a device with no
// config file (or a corrupt one) looks exactly as it always has.
const DashLayout& dashDefaultLayout();

// Parse the config text. Format is one field per line, `#` starts a comment:
//
//     power3s  hero
//     hr       medium  half
//     cadence  medium  half
//     ridetime medium  half
//     distance medium  half
//
// A line reading `page` starts a new page; `page music` adds the media-controls
// page. Text with no `page` lines parses to exactly one page, so every config
// written before pages existed still means what it meant.
//
// Deliberately forgiving — this is a file riders edit by hand on the SD card.
// Unknown field names and sizes are SKIPPED rather than failing the whole file,
// so one typo costs one row instead of the layout. Field pages that end up
// empty are dropped. Returns false only when the result would be no pages,
// which is the caller's cue to keep the default.
bool dashParse(const char* text, DashLayout& out);
bool dashParsePages(const char* text, DashPages& out);

// Write the layout back as the same text, including the header comment. Returns
// the number of bytes written (excluding the terminator), or 0 if it did not fit.
size_t dashSerialize(const DashLayout& layout, char* out, size_t cap);
size_t dashSerializePages(const DashPages& pages, char* out, size_t cap);

// One page of dashDefaultLayout() — what a device with no config shows.
const DashPages& dashDefaultPages();

// Stable identifiers used in the config file and over BLE ("power3s").
const char* dashFieldId(uint8_t field);
const char* dashSizeId(uint8_t size);

// Display name for the cell caption ("POWER · 3S") and for the app's picker.
const char* dashFieldLabel(uint8_t field);

// Look up by id; returns DF_COUNT / DZ_COUNT when unknown.
uint8_t dashFieldFromId(const char* id, size_t len);
uint8_t dashSizeFromId(const char* id, size_t len);
