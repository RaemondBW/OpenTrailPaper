#pragma once

// Pure screen renderers implementing the "Eink Bike GPS" design spec:
// 540x960 portrait, pure black on white, heavy condensed numerals in
// bordered cells, tracked uppercase labels, static chrome.
//
// epdiy framebuffer in, pixels out. No hardware, no tasks — compiled
// on-device AND on the host (tools/preview) for pixel-identical PNGs.

#include <cstdint>

#include "epdiy.h"
#include "dash_layout.h"

struct RideState;
struct RideSummary;

// Shared design-system pieces (also used by map_view.cpp)
namespace ui {

// ---------------------------------------------------------------------------
// Design system v0.1 (docs/dashboard-design-brief.md -> "Device UI System")
//
// One 12 px step, three fixed bands. The screen is always status / body /
// footer; only the body changes between screens, which is what keeps the
// fast-refresh dirty rectangles small and predictable.
// ---------------------------------------------------------------------------
constexpr int STEP = 12;            // base step; every edge is a multiple
constexpr int HALF_STEP = 6;        // rules, insets
constexpr int MARGIN = 24;          // screen margin (2 steps)
constexpr int GUTTER = 12;          // between cells
constexpr int CELL_PAD = 16;        // inside a cell
constexpr int CONTENT_W = 492;      // 540 - 2*MARGIN
constexpr int CONTENT_X = MARGIN;   // body runs x: 24 -> 516

constexpr int STATUS_H = 64;   // status band, incl. its 3 px rule
constexpr int FOOTER_H = 150;  // footer band
constexpr int MAP_STRIP_TOP = 810;  // map viewport is [STATUS_H, MAP_STRIP_TOP);
                                    // the 3-cell data footer sits below it
constexpr int BODY_TOP = STATUS_H;
constexpr int BODY_BOTTOM = MAP_STRIP_TOP;

constexpr int ROW_H = 148;       // list row
// Settings row. Sized by what the page has to hold rather than picked: seven
// setting rows plus the GPS DEBUG nav row have to land above kContentBottom
// (936), and 64 + 8 * 108 = 928 clears it. Was 111 when the page carried six
// settings rows and started 32 px lower. The floor is TOUCH_MIN (88) plus
// enough air that the stepper does not crowd the row rule — 108 leaves 10 px
// above and below an 88 px target, against 11 px before.
constexpr int DENSE_ROW_H = 108; // settings row
constexpr int TOUCH_MIN = 88;    // minimum hit rect, one gloved thumb

// Ink. Four levels exist, but only INK and PAPER survive a fast DU pass — mid
// greys are where ghosting settles. So DARK/LIGHT are STATIC CHROME ONLY: legal
// on a GC16 frame, never inside a 1 Hz region. Anything that must express tone
// while changing uses a screentone instead (see tone.. below).
//
// NOTE: the byte holds two pixels, so a "colour" here is a doubled nibble —
// 0x22 is nibble 2, not 34/255. This project has previously observed that
// nibbles at or above 4 read close to white on this panel; the system's DARK is
// therefore the value most in need of checking on real glass.
constexpr uint8_t INK = 0x00;
constexpr uint8_t DARK = 0x22;    // secondary chrome (system calls this 0x55)
constexpr uint8_t LIGHT = 0x33;   // disabled rows (system calls this 0xAA)
constexpr uint8_t PAPER = 0xFF;   // absence of drive, never a "background"

constexpr int RULE = 2;           // standard rule / border weight
constexpr int RULE_HEAVY = 3;     // band separators

// 1-bit screentones, for tone inside a region that changes.
enum Tone : uint8_t {
    TONE_25,   // 4 px dot     — water
    TONE_30,   // 45 deg hatch — parks
    TONE_50,   // 2 px checker — selected row, pressed button, scrim
    TONE_33,   // scanline     — stale / no-signal values
};
void fillTone(const EpdRect& r, Tone t, uint8_t* fb);

// The CELL primitive: border + padding + label over value with a baseline
// aligned unit. `unit` may be empty; `stale` swaps the value for a dash on a
// scanline tone, which is how "no data" is said (never an empty field).
// `forced` pins the value face. Cells of the same configured size must render
// at the same size — otherwise a wide value like "1:47:12" quietly drops a step
// and sits smaller than "54.8" in an identical box. Pass nullptr to let the
// cell fit its own value (used where there is only one).
void cell(const EpdRect& r, const char* label, const char* value,
          const char* unit, uint8_t* fb, bool stale = false,
          const EpdFont* forced = nullptr,
          const EpdFont* forcedLabel = nullptr);

// Label faces, largest first — same idea as kValueLadder, so a caller can pin
// one caption size across a group of matching cells.
constexpr int LABEL_LADDER_N = 3;
extern const EpdFont* const kLabelLadder[LABEL_LADDER_N];

// The ladder cells step down through, largest first. Exposed so a caller can
// pick one face for a whole group of same-sized cells. It reaches the hero
// sizes at the top: a cell occupying a third of the panel must be able to fill
// it, not stop at the same face a quarter-height cell uses.
constexpr int VALUE_LADDER_N = 9;
extern const EpdFont* const kValueLadder[VALUE_LADDER_N];
int valueFontIndex(const EpdFont* const* ladder, const char* value, int availW,
                   int availH, int unitW);

// `title` is drawn centred in the band — menu, settings and the list screens
// wear the SAME status bar as the ride and map screens, with their name in it,
// rather than each inventing a header of its own.
void statusBar(const RideState& s, uint8_t* fb, const char* title = nullptr);

void text(const EpdFont* font, int x, int y, const char* str, uint8_t* fb,
          EpdFontFlags align = EPD_DRAW_ALIGN_LEFT, uint8_t color = 0x00);
int textWidth(const EpdFont* font, const char* str);
// Width of the same string as label() would draw it (textWidth + tracking).
int labelWidth(const EpdFont* font, const char* str);

// Tracked-out uppercase label, e.g. "POWER · 3S" (design letterspacing).
// font = nullptr uses the standard 14 pt label font; pass a bigger font
// for headers/buttons.
void label(int cx, int y, const char* str, uint8_t* fb, uint8_t color = 0x00,
           const EpdFont* font = nullptr);

// Big value centered in [x0,x1] with a small unit suffix at the baseline.
void valueWithUnit(const EpdFont* valueFont, int x0, int x1, int baselineY,
                   const char* value, const char* unit, uint8_t* fb,
                   uint8_t color = 0x00);

}  // namespace ui

// Main ride screen. The fields, their order and their sizes come from `layout`
// (see dash_layout.h) rather than being baked in; dashDefaultLayout() reproduces
// the original power-hero-over-2x2-grid design exactly. When navActive, the top
// turn banner is showing, so the fields are packed into the space below it.
void ui_render_dashboard(const RideState& s, bool navActive,
                         const DashLayout& layout, uint8_t* fb);

// Did the last ui_render_dashboard() grey out any cell?
//
// The caller needs this to know whether the panel wants a scrub. The greyed-out
// tone is a 25% dot — fine alternating pixels — and the driver's delta engine
// records a black that only half-erased as white, so residue under a newly
// toned area is never touched again and the cell reads as dirty rather than
// inactive. Same problem the map transition already scrubs for.
bool ui_render_dashboard_toned();

// Ride summary (design 1g) with SAVE RIDE / DISCARD touch targets.
extern const EpdRect kResumeButton;
extern const EpdRect kSaveButton;
extern const EpdRect kDiscardButton;
void ui_render_summary(const RideSummary& r, uint8_t* fb);

// Menu (design 1h). Rows are kMenuRowH tall starting at kMenuRowTop;
// row 0 (Start/Stop Ride) is the only action today, the rest show live
// status. Tapping outside the rows returns to the ride screen.
//
// Rows butt straight onto the status band. This was 96, which left a 32 px
// band of nothing under a status bar that already ends in its own 3 px rule
// (see statusBar) — and every page then drew a SECOND heavy rule at the top of
// its rows, so the gap read as an empty stripe between two lines rather than as
// deliberate space. One separator is enough, and the status bar owns it.
constexpr int kMenuRowTop = ui::STATUS_H;
constexpr int kMenuRowH = ui::ROW_H;
constexpr int kMenuRowCount = 5;

struct MenuInfo {
    bool recording = false;
    bool gpsReady = false;
    bool sdOk = false;
    int rideCount = 0;
    uint32_t sdFreeMB = 0;
    bool hr = false, pwr = false, cad = false;
    uint8_t batteryPercent = 0;
    double rideDistanceM = 0;
    bool useMiles = false;
    char routeLine[40] = "no route loaded";
};
void ui_render_menu(const MenuInfo& m, uint8_t* fb);

// Generic list screen (Sensors / Navigate / Ride History). Same row
// geometry as the menu; tap-outside-rows returns.
struct ListRow {
    char title[40];
    char subtitle[64];
    bool inverted;
};
// `turnArrows` puts a maneuver arrow at the head of every row and drops the
// right-edge marker — used by the directions list, where each row IS a turn.
void ui_render_list(const char* title, const ListRow* rows, int count,
                    const char* footer, uint8_t* fb, bool turnArrows = false);

// Settings editor: +/- touch targets per row, geometry exported for the
// touch handler.
// STEPPER — two 88 x 88 targets with the value centred between them, the block
// right-aligned in the content column. 88 is the system's minimum target for a
// gloved thumb; 3 x 88 = 264 keeps every edge on the 12 px step.
constexpr int kSettingsBtn = 88;
constexpr int kSettingsPlusX =
    ui::CONTENT_X + ui::CONTENT_W - ui::CELL_PAD - kSettingsBtn;
// Specimen: [88 target] gap16 [100 px value well] gap16 [88 target], the group
// right-aligned in the row's 16 px padding.
constexpr int kSettingsValueW = 100;
constexpr int kSettingsGap = 16;
constexpr int kSettingsMinusX =
    kSettingsPlusX - kSettingsGap - kSettingsValueW - kSettingsGap - kSettingsBtn;
// SWITCH — drawn 120 x 52, hit rect 120 x 88. The drawn shape stays small; the
// target does not.
constexpr int kSettingsToggleW = 120;
constexpr int kSettingsToggleH = 52;
constexpr int kSettingsToggleX =
    ui::CONTENT_X + ui::CONTENT_W - ui::CELL_PAD - kSettingsToggleW;
// UNITS is wider than the ON/OFF switches because its two positions carry WORDS
// — "MILES" in a 60 px half was set solid against both edges. Only the width
// changes: the right edge stays on the same column as every other switch, so
// the control column still reads as one line down the page.
constexpr int kSettingsUnitsToggleW = 168;
constexpr int kSettingsUnitsToggleX =
    ui::CONTENT_X + ui::CONTENT_W - ui::CELL_PAD - kSettingsUnitsToggleW;
// Per-row switch geometry, so the hit test and the drawing cannot disagree.
constexpr int kSettingsUnitsRowIdx = 3;
constexpr int settingsToggleW(int row) {
    return row == kSettingsUnitsRowIdx ? kSettingsUnitsToggleW : kSettingsToggleW;
}
constexpr int settingsToggleX(int row) {
    return row == kSettingsUnitsRowIdx ? kSettingsUnitsToggleX : kSettingsToggleX;
}
constexpr int kSettingsRowH = ui::DENSE_ROW_H;
struct SettingsInfo {
    bool showOffline;
    int ftpW;
    int tzMin;
    int backlight;   // 0 off .. 3 bright
    bool useMiles;   // false = km, true = miles
    bool usbDrive;   // expose the SD to a host as a USB drive
    bool meshOn;     // LoRa mesh radio powered up and listening
};
void ui_render_settings(const SettingsInfo& si, uint8_t* fb);

// Sub-screens run their content to here. There is no BACK strip: the capacitive
// Home button below the glass already goes back from every one of them, so a
// full-width control repeating it cost 96 px on every list and settings page to
// duplicate a key the rider's thumb is already on.
constexpr int kContentBottom = 960 - ui::MARGIN;

// Settings sub-page row hit-testing. Rows 0-2 are +/- steppers (FTP, timezone,
// backlight — four backlight levels do not fit a switch); rows 3-6 are toggle
// switches (units, USB drive, show offline, mesh); row 7 is navigation.
// Sensors lives on the main menu, not here.
constexpr int kSettingsBacklightRow = 2;
constexpr int kSettingsUnitsRow = kSettingsUnitsRowIdx;
constexpr int kSettingsUsbRow = 4;
constexpr int kSettingsOfflineRow = 5;
constexpr int kSettingsMeshRow = 6;
constexpr int kSettingsRowCount = 7;   // stepper + switch rows, excluding nav
constexpr int kSettingsGpsRow = kSettingsRowCount;

// GPS diagnostics page (reached from Settings). Mirrors GpsDebug from
// gps_service.h but stays host-safe for the preview harness.
struct GpsDebugView {
    bool moduleDetected;
    uint32_t chars, passedCksum, failedCksum, withFix;
    int satsInUse, satsInView;
    int bestSnr;
    float hdop;
    bool locValid;
    uint32_t locAgeMs;
    double lat, lon;
    float altM, speedKmh;
    int hour, minute, second;
    bool useMiles;
    const char* module;   // detected chipset name
};
void ui_render_gps_debug(const GpsDebugView& g, uint8_t* fb);

// Power dialog — a bottom sheet drawn over whatever screen is behind it
// (long-press of a physical button). Touch targets:
extern const EpdRect kPowerSheet;      // sheet region; taps inside are kept
extern const EpdRect kPowerShutdown;   // SHUT DOWN button
extern const EpdRect kPowerCancel;     // CANCEL button
void ui_render_power_sheet(bool recording, uint8_t* fb);

// Static farewell left on the glass through deep sleep.
void ui_render_shutdown_screen(uint8_t* fb);

// "Start navigation?" bottom sheet shown when a route with turn cues
// arrives. Reuses the power-sheet button rects: kPowerShutdown = START,
// kPowerCancel = LATER.
// Boot progress. `lines`/`state` are parallel arrays, one entry per step
// started so far. state[i] is BOOT_PENDING while the step is still running,
// then BOOT_OK (tick) or BOOT_FAIL (cross). A step is drawn as soon as it
// STARTS, so a slow one (the SD mount can take seconds) shows what the device
// is waiting on instead of leaving the screen frozen on the previous step.
enum : int8_t { BOOT_FAIL = -1, BOOT_PENDING = 0, BOOT_OK = 1 };
// Technical boot log: one line per step, kernel-style.
//   [  1.87] SD CARD  30436 MB free            [ OK ]
// `detail` and `ms` are parallel to `lines`/`state`; detail may hold "" and ms
// is the millis() at which the step resolved (0 while it is still running).
void ui_render_boot_screen(const char* version, const char* const* lines,
                           const int8_t* state, const char (*detail)[28],
                           const uint32_t* ms, int count, uint8_t* fb);

void ui_render_nav_prompt(const char* routeName, int turns, uint8_t* fb);

// Full-screen "Updating firmware" modal shown while an OTA is in progress.
void ui_render_update_overlay(const char* phase, int pct, uint8_t* fb);

// Turn-by-turn banner drawn over the top of the map while navigating:
// distance to the next turn + the instruction, with a direction arrow.
void ui_render_nav_banner(const char* instruction, float distanceM,
                          bool useMiles, uint8_t* fb);

// Auto-pause banner, same band as the turn banner (the turn banner wins when
// both apply): pause glyph, "AUTO-PAUSED", tap-to-resume hint. Static by
// design so a paused screen stops costing panel refreshes.
void ui_render_pause_banner(uint8_t* fb);
