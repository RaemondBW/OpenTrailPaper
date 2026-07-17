#pragma once

// Pure screen renderers implementing the "Eink Bike GPS" design spec:
// 540x960 portrait, pure black on white, heavy condensed numerals in
// bordered cells, tracked uppercase labels, static chrome.
//
// epdiy framebuffer in, pixels out. No hardware, no tasks — compiled
// on-device AND on the host (tools/preview) for pixel-identical PNGs.

#include <cstdint>

#include "epdiy.h"

struct RideState;
struct RideSummary;

// Shared design-system pieces (also used by map_view.cpp)
namespace ui {

constexpr int STATUS_H = 64;   // status bar height incl. 3 px rule

void statusBar(const RideState& s, uint8_t* fb);

void text(const EpdFont* font, int x, int y, const char* str, uint8_t* fb,
          EpdFontFlags align = EPD_DRAW_ALIGN_LEFT, uint8_t color = 0x00);
int textWidth(const EpdFont* font, const char* str);

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

// Main ride screen (design 1a: power hero + grid; falls back to a speed
// hero when no power meter is connected). When navActive, the top turn
// banner is showing, so the hero is drawn smaller and below it.
void ui_render_dashboard(const RideState& s, bool navActive, uint8_t* fb);

// Ride summary (design 1g) with SAVE RIDE / DISCARD touch targets.
extern const EpdRect kResumeButton;
extern const EpdRect kSaveButton;
extern const EpdRect kDiscardButton;
void ui_render_summary(const RideSummary& r, uint8_t* fb);

// Menu (design 1h). Rows are kMenuRowH tall starting at kMenuRowTop;
// row 0 (Start/Stop Ride) is the only action today, the rest show live
// status. Tapping outside the rows returns to the ride screen.
constexpr int kMenuRowTop = 96;
constexpr int kMenuRowH = 148;
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
void ui_render_list(const char* title, const ListRow* rows, int count,
                    const char* footer, uint8_t* fb);

// Settings editor: +/- touch targets per row, geometry exported for the
// touch handler.
constexpr int kSettingsMinusX = 220;
constexpr int kSettingsPlusX = 440;
constexpr int kSettingsBtn = 84;
// The settings screen packs more rows than the menu, so it uses a shorter row.
constexpr int kSettingsRowH = 95;
struct SettingsInfo {
    int ftpW;
    int tzMin;
    int backlight;   // 0 off .. 3 bright
    bool useMiles;   // false = km, true = miles
    bool usbDrive;   // expose the SD to a host as a USB drive
};
void ui_render_settings(const SettingsInfo& si, uint8_t* fb);

// Full-width tappable BACK strip along the bottom of every sub-screen.
extern const EpdRect kBackBar;
void ui_render_back_bar(uint8_t* fb);

// Settings sub-page row hit-testing. Rows 0-4 are +/- adjusters (FTP,
// timezone, frontlight, units, USB drive); rows 5-7 are navigation.
constexpr int kSettingsBacklightRow = 2;
constexpr int kSettingsUnitsRow = 3;
constexpr int kSettingsUsbRow = 4;
constexpr int kSettingsSensorsRow = 5;
constexpr int kSettingsGpsRow = 6;
constexpr int kSettingsGreyRow = 7;

// Grayscale test screen: labelled swatches from white to black to pick which
// gray levels reproduce on the panel.
void ui_render_grey_test(uint8_t* fb);

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
void ui_render_nav_prompt(const char* routeName, int turns, uint8_t* fb);

// Full-screen "Updating firmware" modal shown while an OTA is in progress.
void ui_render_update_overlay(const char* phase, int pct, uint8_t* fb);

// Turn-by-turn banner drawn over the top of the map while navigating:
// distance to the next turn + the instruction, with a direction arrow.
void ui_render_nav_banner(const char* instruction, float distanceM,
                          bool useMiles, uint8_t* fb);
