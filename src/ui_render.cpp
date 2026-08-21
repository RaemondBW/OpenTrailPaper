#include "ui_render.h"
#include "epd_compat.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "config.h"
#include "ride_state.h"
// Faces at the design system's ACTUAL sizes. The drop's specimens are drawn 1:1
// with the panel, so its CSS px are device px — and the previously compiled
// faces were 2.1-2.4x those sizes (Impact_40 rendered a 68 px cap where the
// drop's 40 px Impact wants 29). Everything was therefore roughly double the
// intended scale, which is why captions collided and rows had to be truncated.
//
//   Arial_L   label      = drop's arialbold_14   (cap 11)
//   Arial_B   body       = drop's arialbold_20   (cap 15)
//   Impact_S  clock      = drop's impact_28      (cap 20)
//   Impact_T  row/button = drop's impact_40      (cap 29)
//   Impact_V  cell value = drop's impact_64      (cap 46)
//   Impact_H  hero       = drop's impact_128     (cap 95)
#include "fonts/arial_l.h"
#include "fonts/arial_b.h"
#include "fonts/impact_s.h"
#include "fonts/impact_t.h"
#include "fonts/impact_m.h"
#include "fonts/impact_v.h"
#include "fonts/impact_h.h"
#include "fonts/impact_a.h"
#include "fonts/impact_b.h"
#include "fonts/impact_c.h"
#include "fonts/impact_xl.h"
#include "fonts/arialbold_14.h"
#include "fonts/arialbold_20.h"
#include "fonts/impact_40.h"
#include "fonts/impact_128.h"

// Summary footer touch targets (design 1g: two 100+ px tall actions)
// BUTTON — h 96, three across the content column with a 12 px gutter:
// (492 - 2*12) / 3 = 156. Hit rects are the drawn rects here, both past the
// 88 px minimum.
// SHEET buttons: SAVE and DISCARD side by side (gap 12), RESUME full width
// beneath them (gap 20). All 96 px tall, inset on the sheet's 24 px padding.
const EpdRect kSaveButton    = {24, 700, 240, 96};
const EpdRect kDiscardButton = {276, 700, 240, 96};
const EpdRect kResumeButton  = {24, 816, 492, 96};

namespace {
// Defined further down with the other formatters; declared here so the status
// bar and the CLOCK dashboard field share one implementation.
void formatClock(char* out, size_t len, const RideState& s);
}

namespace ui {

void text(const EpdFont* font, int x, int y, const char* str, uint8_t* fb,
          EpdFontFlags align, uint8_t color) {
    EpdFontProperties props = epd_font_properties_default();
    props.flags = align;
    props.fg_color = color >> 4;
    epd_write_string(font, str, &x, &y, fb, &props);
}

int textWidth(const EpdFont* font, const char* str) {
    EpdFontProperties props = epd_font_properties_default();
    EpdRect r = epd_get_string_rect(font, str, 0, 0, 0, &props);
    return r.width;
}

// Tracked-out label: draws characters individually with a few extra
// pixels of letterspacing, matching the design. UTF-8 aware.
// Rendered width of a tracked label. Same arithmetic as label() below, exposed
// so callers can pick a font that FITS before drawing — tracking adds 3 px per
// character, so a plain textWidth() underestimates by enough to overflow a
// half-width cell on a long caption like "MOVING TIME".
int labelWidth(const EpdFont* font, const char* str) {
    if (!font) font = &Arial_L;
    constexpr int TRACK = 3;
    char ch[8];
    int total = -TRACK;
    for (const char* p = str; *p;) {
        int len = 1;
        if (*p & 0x80) { while ((p[len] & 0xC0) == 0x80) len++; }
        memcpy(ch, p, len);
        ch[len] = 0;
        p += len;
        total += textWidth(font, ch) + TRACK;
    }
    return total < 0 ? 0 : total;
}

void label(int cx, int y, const char* str, uint8_t* fb, uint8_t color,
           const EpdFont* font) {
    if (!font) font = &Arial_L;
    constexpr int TRACK = 3;
    char ch[8];
    auto nextChar = [&](const char*& p) {
        int len = 1;
        if (*p & 0x80) {
            while ((p[len] & 0xC0) == 0x80) len++;
        }
        memcpy(ch, p, len);
        ch[len] = 0;
        p += len;
    };

    int total = -TRACK;
    for (const char* p = str; *p;) {
        nextChar(p);
        total += textWidth(font, ch) + TRACK;
    }

    int x = cx - total / 2;
    for (const char* p = str; *p;) {
        nextChar(p);
        text(font, x, y, ch, fb, EPD_DRAW_ALIGN_LEFT, color);
        x += textWidth(font, ch) + TRACK;
    }
}

void valueWithUnit(const EpdFont* valueFont, int x0, int x1, int baselineY,
                   const char* value, const char* unit, uint8_t* fb,
                   uint8_t color) {
    int vw = textWidth(valueFont, value);
    int uw = unit && unit[0] ? textWidth(&Arial_L, unit) + 10 : 0;
    int startX = x0 + ((x1 - x0) - (vw + uw)) / 2;
    if (startX < x0) startX = x0;   // never spill past the left bound / screen edge
    text(valueFont, startX, baselineY, value, fb, EPD_DRAW_ALIGN_LEFT, color);
    if (uw) {
        text(&Arial_L, startX + vw + 10, baselineY, unit, fb,
             EPD_DRAW_ALIGN_LEFT, color);
    }
}

namespace {

// Checkmark drawn with primitives (the font has no U+2713).
void check(int x, int y, uint8_t* fb, uint8_t color = 0x00) {
    for (int t = 0; t < 3; ++t) {
        epd_draw_line(x, y - 6 + t, x + 5, y - 1 + t, color, fb);
        epd_draw_line(x + 5, y - 1 + t, x + 14, y - 14 + t, color, fb);
    }
}

void batteryIcon(int rightX, int cy, uint8_t percent, bool charging,
                 uint8_t* fb, uint8_t color = 0x00) {
    const int w = 44, h = 24;
    int x = rightX - w - 6;
    EpdRect body = {x, cy - h / 2, w, h};
    epd_draw_rect(body, color, fb);
    EpdRect body2 = {x + 1, cy - h / 2 + 1, w - 2, h - 2};
    epd_draw_rect(body2, color, fb);
    EpdRect tip = {x + w, cy - 5, 5, 10};
    epd_fill_rect(tip, color, fb);
    int fillW = (w - 8) * percent / 100;
    if (fillW > 0) {
        EpdRect fill = {x + 4, cy - h / 2 + 4, fillW, h - 8};
        epd_fill_rect(fill, color, fb);
    }
    (void)charging;
}

// Small downward lightning bolt centered at (cx, cy) — the "charging" glyph.
void drawBolt(int cx, int cy, uint8_t color, uint8_t* fb) {
    epd_fill_triangle(cx + 4, cy - 9, cx - 3, cy + 2, cx + 2, cy + 1, color, fb);
    epd_fill_triangle(cx - 4, cy + 9, cx + 3, cy - 2, cx - 2, cy - 1, color, fb);
}

}  // namespace

void statusBar(const RideState& s, uint8_t* fb, const char* title) {
    const int W = epd_rotated_display_width();

    // Clock from GPS time (local), 24h or 12h per the setting.
    char clock[10];
    formatClock(clock, sizeof(clock), s);
    const int clockX = 16;
    text(&Impact_S, clockX, 41, clock, fb);
    // Lay the rest of the left cluster out FROM the clock's measured width. It
    // was hardcoded at x=96, which is fine for "14:25" but not for the 12-hour
    // format: "12:45p" is a character wider and ran straight into the phone
    // glyph. Widths differ per string ("1:05a" vs "12:45p"), so measure.
    int leftX = clockX + textWidth(&Impact_S, clock) + 14;

    // Companion-app connection: a small phone glyph just after the clock (on the
    // left, out of the crowded battery cluster). Absent = not connected.
    if (s.phoneConnected) {
        const int pw = 15, ph = 26, px = leftX, py = 30 - ph / 2;
        epd_fill_rect({px, py, pw, ph}, 0x00, fb);            // phone body
        epd_fill_rect({px + 3, py + 4, pw - 6, ph - 10}, 0xFF, fb);  // screen
        epd_fill_circle(px + pw / 2, py + ph - 4, 1, 0xFF, fb);      // home dot
    }

    // GPS signal dots + sensor labels, left-anchored after the clock/phone. The
    // "GPS" text label is dropped — the dots read as signal strength and the
    // saved width keeps "· PWR" clear of the battery %.
    if (s.phoneConnected) leftX += 15 + 14;   // phone glyph width + gap
    // With a page title the band is clock | TITLE | battery: the GPS dots are
    // dropped so the name has the middle to itself. They are ride status, not
    // something a menu needs.
    int dotsW = title ? 0 : 4 * 16;
    int x = leftX;
    int bars = s.gpsFix ? (s.satellites >= 9 ? 4 : s.satellites >= 6 ? 3
                           : s.satellites >= 4 ? 2 : 1)
                        : 0;
    if (!title)
        for (int i = 0; i < 4; ++i) {
            if (i < bars) epd_fill_circle(x + i * 16, 32, 5, 0x00, fb);
            else epd_draw_circle(x + i * 16, 32, 5, 0x00, fb);
        }
    x += dotsW;
    // The "· HR" / "· PWR" labels only appear when connected, so they need no
    // extra checkmark — keeping them text-only frees room for the battery %.
    if (s.hrConnected) {
        if (!title) text(&Arial_L, x, 40, " · HR", fb);
        x += textWidth(&Arial_L, " · HR");
    }
    if (s.powerConnected) {
        if (!title) text(&Arial_L, x, 40, " · PWR", fb);
        x += textWidth(&Arial_L, " · PWR");
    }
    // Auto-pause: the frozen ride timer is the real signal, but a frozen number
    // needs a caption or it reads as a hang. Same label cluster as HR/PWR.
    if (s.ridePaused) {
        if (!title) text(&Arial_L, x, 40, " · PAUSED", fb);
        x += textWidth(&Arial_L, " · PAUSED");
    }

    batteryIcon(W - 12, 30, s.batteryPercent, s.charging, fb);

    // Numeric battery level left of the icon — the fill bar alone is hard to
    // read on e-paper. Battery body left edge = (W-12) - 44 - 6 = W-62. A
    // reading of 0 means "not yet measured" (a real 0% would have shut down),
    // so show "--" rather than a bogus 0% right after boot/install.
    char pct[8];
    if (s.batteryPercent == 0) snprintf(pct, sizeof(pct), "--%%");
    else snprintf(pct, sizeof(pct), "%u%%", s.batteryPercent);
    int pctRight = W - 62 - 8;
    text(&Impact_S, pctRight, 41, pct, fb, EPD_DRAW_ALIGN_RIGHT);

    // Lightning bolt left of the % when charging.
    if (s.charging) {
        int pctLeft = pctRight - textWidth(&Impact_S, pct);
        drawBolt(pctLeft - 10, 30, 0x00, fb);
    }

    epd_fill_rect({0, STATUS_H - 3, W, 3}, 0x00, fb);
    // Centred title, drawn last. The clusters are short (clock left, battery
    // right), so a page name fits between them.
    if (title && title[0])
        label(epd_rotated_display_width() / 2, 41, title, fb, INK, &Impact_S);
}


// --- Screentones ----------------------------------------------------------
// Tone, not grey. A mid grey inside a 1 Hz region ghosts (the driver's model
// loses lightened pixels), so anything that must read as "less than black"
// while it changes is built from 1-bit pixels instead.
void fillTone(const EpdRect& r, Tone t, uint8_t* fb) {
    for (int y = r.y; y < r.y + r.height; ++y) {
        for (int x = r.x; x < r.x + r.width; ++x) {
            bool on = false;
            switch (t) {
                // 25%: one pixel in every 2x2. This was one in every 4x4 —
                // 6.25%, four times lighter than the name says, which is why a
                // greyed-out cell barely read as greyed out at all.
                case TONE_25: on = ((x & 1) == 0) && ((y & 1) == 0); break;
                case TONE_30: on = ((x + y) % 3) == 0; break;         // 45 deg
                case TONE_50: on = ((x >> 1) + (y >> 1)) % 2 == 0; break;
                case TONE_33: on = (y % 3) == 0; break;               // scanline
            }
            if (on) epd_fill_rect({x, y, 1, 1}, INK, fb);
        }
    }
}

// --- CELL -----------------------------------------------------------------
// Specimen: 240 x 150, border 2, pad 16. Label top-LEFT (arialbold_14, tracked),
// value left-aligned 10 px below it in impact, unit baseline-aligned 6 px along.
// Nothing here is centred — the drop aligns the whole cell to its left padding.
// Fine-grained on purpose: the rider asked for values as large as the box
// allows, and a coarse ladder lands well under the ceiling (46 -> 69 -> 95 left
// a 60 px gap where a 66 px face would have fitted).
//
// STRICTLY DESCENDING by digit height, and it has to stay that way — the two
// things that read this ladder both assume it. valueFontIndex() returns the
// FIRST face that fits, so an out-of-order entry hands back a smaller face than
// the cell could have taken; worse, the dashboard equalises a size class by
// keeping the LARGEST INDEX any of its cells needed, which is only "the
// smallest face" if the order holds — otherwise a class renders at a face too
// big for one of its cells and the value overflows the box. Impact_H (95) sat
// ahead of Impact_C (120), so both happened at once.
// Heights: 158, 120, 95, 81, 69, 58, 46, 30, 15.
const EpdFont* const kValueLadder[VALUE_LADDER_N] = {
    &Impact_XL, &Impact_C, &Impact_H, &Impact_B, &Impact_M,
    &Impact_A,  &Impact_V, &Impact_T, &Arial_B};

// Index of the largest ladder face that fits; 3 (the smallest) if none do.
int valueFontIndex(const EpdFont* const* ladder, const char* value, int availW,
                   int availH, int unitW) {
    for (int i = 0; i < VALUE_LADDER_N; ++i) {
        if (epdc_digit_height(ladder[i]) <= availH &&
            textWidth(ladder[i], value) + unitW <= availW)
            return i;
    }
    return VALUE_LADDER_N - 1;
}

const EpdFont* const kLabelLadder[LABEL_LADDER_N] = {&ArialBold_20, &Arial_B,
                                                     &Arial_L};

void cell(const EpdRect& r, const char* labelStr, const char* value,
          const char* unit, uint8_t* fb, bool stale, const EpdFont* forced,
          const EpdFont* forcedLabel) {
    for (int b = 0; b < RULE; ++b)
        epd_draw_rect({r.x + b, r.y + b, r.width - 2 * b, r.height - 2 * b}, INK, fb);

    // NO DATA: scanline tone behind the VALUE only. Running it across the whole
    // cell (as a 10%-opacity wash does on screen) buried the label here — this
    // is 1-bit, so the tone is as black as the type sitting on it.
    // A field whose sensor is not connected is GREYED OUT: the system's dot tone
    // across the whole cell, drawn first so the caption and the no-data rule sit
    // on top of it in ink. The dot (25%, 4 px pitch) rather than the scanline —
    // the scanline is dense enough to bury a caption at this size, and the point
    // is to read as inactive, not as damaged.
    if (stale)
        fillTone({r.x + RULE, r.y + RULE, r.width - 2 * RULE, r.height - 2 * RULE},
                 TONE_25, fb);
    const bool toneValue = false;

    const int lx = r.x + CELL_PAD;
    const int ly = r.y + CELL_PAD + Arial_B.ascender + 6;
    // Labels step down too. "HEART RATE" at 20 pt is wider than a 240 px cell,
    // and a caption that runs into its neighbour is worse than a smaller one.
    const int labelAvail = r.width - 2 * CELL_PAD;
    const EpdFont* lf = forcedLabel;
    if (!lf) {
        lf = kLabelLadder[LABEL_LADDER_N - 1];
        for (int i = 0; i < LABEL_LADDER_N; ++i)
            if (labelWidth(kLabelLadder[i], labelStr) <= labelAvail) {
                lf = kLabelLadder[i];
                break;
            }
    }
    const int lw = labelWidth(lf, labelStr);
    label(lx + lw / 2, ly, labelStr, fb, INK, lf);

    // Value box: everything below the label, less the bottom padding.
    const int top = ly + 10;
    const int bot = r.y + r.height - CELL_PAD;
    const int avail = r.width - 2 * CELL_PAD;
    const int unitW = (unit && unit[0]) ? textWidth(&Arial_B, unit) + 4 : 0;

    // Impact_M (the sheet face) is the top step: on real glass the drop's cell
    // size read small, and a tall cell has the room for it.
    const EpdFont* vf = forced;
    if (!vf) {
        const int idx = valueFontIndex(kValueLadder, value, avail, bot - top, unitW);
        vf = kValueLadder[idx];
    }
    int vh = epdc_digit_height(vf);
    // CENTRE the value in the space below the label. The specimen hangs it off
    // the bottom padding, which is right in a 150 px cell where the number all
    // but fills the box — but wrong the moment a cell is tall (a field left
    // alone after its neighbours' sensors dropped out inherits the whole
    // panel), where it stranded the value at the very bottom under a void.
    const int baseline = top + ((bot - top) + vh) / 2;

    // The value and its unit are ONE object, centred in the cell as a pair —
    // centring the number alone would push it off-axis by half the unit's
    // width. The label stays pinned top-left; only the figure is centred.
    const int vw = textWidth(vf, value);
    const int startX = r.x + (r.width - vw - unitW) / 2;
    if (toneValue) {
        // NO DATA is drawn, not typed. A hyphen's ink is a short thin bar
        // whatever face it is set in, so "--" at the same font as its
        // neighbours still rendered a fraction of their size and read as a
        // typographic bug. A rule scaled to the digit height sits at the weight
        // the number would have had, which is what makes the cell read as
        // "no source" rather than "broken".
        const int barW = vh * 5 / 8, barH = vh / 6 < 4 ? 4 : vh / 6;
        const int gap = barW / 3;
        const int totalW = barW * 2 + gap;
        const int bx = r.x + (r.width - totalW - unitW) / 2;
        const int by = baseline - vh / 2 - barH / 2;
        epd_fill_rect({bx, by, barW, barH}, INK, fb);
        epd_fill_rect({bx + barW + gap, by, barW, barH}, INK, fb);
        if (unitW)
            text(&Arial_B, bx + totalW + 4, baseline, unit, fb,
                 EPD_DRAW_ALIGN_LEFT, INK);
    } else {
        text(vf, startX, baseline, value, fb, EPD_DRAW_ALIGN_LEFT, INK);
        if (unitW)
            text(&Arial_B, startX + vw + 4, baseline, unit, fb,
                 EPD_DRAW_ALIGN_LEFT, INK);
    }
    (void)vh;
}

}  // namespace ui

namespace {

// Impact carries NO lowercase glyphs, so any caller-supplied string set in it
// silently loses every lowercase character ("Turn left onto Valencia" drew as
// "T V S"). Uppercase before drawing; this has bitten list titles, sheet heroes
// and the nav banner in turn.
void upperCopy(char* out, size_t len, const char* in) {
    size_t n = 0;
    for (; in[n] && n < len - 1; ++n)
        out[n] = (in[n] >= 'a' && in[n] <= 'z') ? (char)(in[n] - 32) : in[n];
    out[n] = 0;
}

void formatHms(char* out, size_t len, uint32_t secs) {
    snprintf(out, len, "%lu:%02lu:%02lu", (unsigned long)(secs / 3600),
             (unsigned long)((secs / 60) % 60), (unsigned long)(secs % 60));
}

// Local time of day, 24 h or 12 h per the rider's setting. Shared by the status
// bar and the CLOCK dashboard field so the two can never disagree about the
// format — they are inches apart on the same screen.
void formatClock(char* out, size_t len, const RideState& s) {
    snprintf(out, len, "--:--");
    if (!s.timeValid) return;
    time_t local = s.utc + (time_t)s.tzMin * 60;
    struct tm tmv;
    gmtime_r(&local, &tmv);
    if (s.clock24h) {
        snprintf(out, len, "%d:%02d", tmv.tm_hour, tmv.tm_min);
    } else {
        int h = tmv.tm_hour % 12;
        if (h == 0) h = 12;
        snprintf(out, len, "%d:%02d%c", h, tmv.tm_min, tmv.tm_hour < 12 ? 'a' : 'p');
    }
}

// Both power fields drive the FTP zone bar and want the same hero treatment.
bool isPowerField(uint8_t f) { return f == DF_POWER3S || f == DF_POWER; }

// Seven FTP zone segments, filled up to the current zone. Only drawn under a
// hero-sized power cell — at grid size the segments are too fine to read.
void drawPowerZoneBar(const RideState& s, int y, uint8_t* fb) {
    const int W = ui::CONTENT_W + 2 * ui::CONTENT_X;
    uint16_t p = s.power3sW != 0xFFFF ? s.power3sW : s.powerW;
    int ftp = s.ftpW;
    int zone = 0;
    if (p != 0xFFFF && ftp > 0) {
        float pct = 100.0f * p / ftp;
        zone = pct < 55 ? 1 : pct < 75 ? 2 : pct < 90 ? 3 : pct < 105 ? 4
               : pct < 120 ? 5 : pct < 150 ? 6 : 7;
    }
    // Seven segments across the hero cell's inner width, on the 12 px step.
    const int barX = ui::CONTENT_X + ui::CELL_PAD;
    const int barW = ui::CONTENT_W - 2 * ui::CELL_PAD;
    const int segW = (barW - 6 * ui::HALF_STEP) / 7;
    for (int i = 0; i < 7; ++i) {
        EpdRect seg = {barX + i * (segW + ui::HALF_STEP), y, segW, 18};
        if (i < zone) epd_fill_rect(seg, 0x00, fb);
        else epd_draw_rect(seg, 0x00, fb);
    }
}

// One bordered grid cell: tracked label on top, big value below.
void cell(int x0, int y0, int x1, int y1, const char* labelStr,
          const char* value, const char* unit, uint8_t* fb) {
    int cx = (x0 + x1) / 2;
    const int labelY = y0 + 36;
    ui::label(cx, labelY, labelStr, fb);
    // Big value centered in the space between the label and the bottom-anchored
    // unit caption; the unit sits at the cell bottom. Always reserve the unit
    // slot (even when there's no unit) so the value baseline lines up across
    // cells — e.g. RIDE TIME aligns with DISTANCE beside it. The Impact_40
    // baseline sits ~half a cap-height below the target center.
    int unitTop = y1 - 28;
    int vcy = (y0 + 46 + unitTop) / 2;
    int vBaseline = vcy + 14;
    // Impact_40's glyphs are ~58 px tall; in short cells (the ride-summary grid)
    // the centered value would ride up into the label, so push the baseline down
    // until the glyph top clears it. Roomy cells (dashboard) are unaffected.
    const int kValueCapH = 58;
    int minBaseline = labelY + 8 + kValueCapH;
    if (vBaseline < minBaseline) vBaseline = minBaseline;
    ui::text(&Impact_T, cx, vBaseline, value, fb, EPD_DRAW_ALIGN_CENTER, 0x00);
    if (unit && unit[0]) {
        ui::text(&Arial_L, cx, y1 - 12, unit, fb, EPD_DRAW_ALIGN_CENTER, 0x00);
    }
}

}  // namespace

// Format one configurable field into value + unit text. Everything here reads
// something the device already measures — a BLE sensor, the recorder, the GPS,
// the map DEM — so a new field is a case here, not new plumbing.
//
// "--" for absent data rather than a zero: a rider glancing down must be able to
// tell "no heart-rate strap paired" from "heart rate is 0".
void dashFieldValue(uint8_t field, const RideState& s, char* val, size_t valCap,
                    const char** unit) {
    *unit = "";
    switch (field) {
        case DF_SPEED: {
            double v = units::speed(s.speedKmh, s.useMiles);
            snprintf(val, valCap, "%.1f", v);
            *unit = units::speedLabel(s.useMiles);
            break;
        }
        case DF_POWER3S:
            if (s.power3sW != 0xFFFF) snprintf(val, valCap, "%u", s.power3sW);
            else snprintf(val, valCap, "--");
            *unit = "W";
            break;
        case DF_POWER:
            if (s.powerW != 0xFFFF) snprintf(val, valCap, "%u", s.powerW);
            else snprintf(val, valCap, "--");
            *unit = "W";
            break;
        case DF_HEART_RATE:
            if (s.heartRateBpm != 0xFF) snprintf(val, valCap, "%u", s.heartRateBpm);
            else snprintf(val, valCap, "--");
            *unit = "BPM";
            break;
        case DF_CADENCE:
            if (s.cadenceRpm != 0xFF) snprintf(val, valCap, "%u", s.cadenceRpm);
            else snprintf(val, valCap, "--");
            *unit = "RPM";
            break;
        case DF_DISTANCE:
            snprintf(val, valCap, "%.1f", units::distM(s.distanceM, s.useMiles));
            *unit = units::distLabel(s.useMiles);
            break;
        case DF_RIDE_TIME:   formatHms(val, valCap, s.elapsedS); break;
        case DF_MOVING_TIME: formatHms(val, valCap, s.movingS); break;
        case DF_CLIMB:
            snprintf(val, valCap, "%.0f", units::elev(s.climbedM, s.useMiles));
            *unit = units::elevLabel(s.useMiles);
            break;
        case DF_GRADE:
            if (s.gradeValid) snprintf(val, valCap, "%.1f", s.gradePercent);
            else snprintf(val, valCap, "--");
            *unit = "%";
            break;
        case DF_ALTITUDE:
            // The barometer when one is fitted, else the map DEM — and never
            // the GPS chip's altitude, which is far too noisy (see the README's
            // "Elevation without a barometer").
            //
            // These are not two rival readings: the DEM is what CALIBRATES the
            // barometer's sea-level reference (aux_sensors), so the barometric
            // number starts from the map's idea of the ground and then resolves
            // the metre-by-metre change the grid cannot — a bridge, an
            // overpass, the climb between two grid posts.
            if (s.baroValid)
                snprintf(val, valCap, "%.0f", units::elev(s.baroAltM, s.useMiles));
            else if (s.mapElevationValid)
                snprintf(val, valCap, "%.0f", units::elev(s.mapElevationM, s.useMiles));
            else snprintf(val, valCap, "--");
            *unit = units::elevLabel(s.useMiles);
            break;
        case DF_BATTERY:
            snprintf(val, valCap, "%u", s.batteryPercent);
            *unit = "%";
            break;
        case DF_SATELLITES:
            snprintf(val, valCap, "%u", s.satellites);
            break;
        case DF_CLOCK: {
            char clk[16];
            formatClock(clk, sizeof(clk), s);
            snprintf(val, valCap, "%s", clk);
            break;
        }
        case DF_ROUTE_LEFT:
            if (s.routeActive)
                snprintf(val, valCap, "%.1f", units::dist(s.routeRemainingKm, s.useMiles));
            else snprintf(val, valCap, "--");
            *unit = units::distLabel(s.useMiles);
            break;
        default:
            snprintf(val, valCap, "--");
            break;
    }
}

// HERO cell — one per screen, never two.
//
// Its own path because impact_128 is not in the shared cell ladder and needs a
// width fallback the grid cells do not: "1:47:12" at impact_128 is 684 px on a
// 540 px panel, so an unchecked hero runs straight off both edges. Falls back
// to impact_40 rather than clipping.
void heroCell(const EpdRect& r, const char* labelStr, const char* value,
              const char* unit, const RideState& s, bool zoneBar, bool isSpeed,
              bool stale, uint8_t* fb) {
    epd_draw_rect(r, ui::INK, fb);
    epd_draw_rect({r.x + 1, r.y + 1, r.width - 2, r.height - 2}, ui::INK, fb);

    // Greyed out exactly like a grid cell when its sensor is gone — the hero
    // used to skip this entirely, so a dropped-out power meter kept the biggest,
    // most authoritative number on the panel looking live. Tone first, content
    // over the top. The zone bar needs no special case: with no power it
    // computes zone 0 and draws seven empty outlines.
    if (stale)
        fillTone({r.x + 2, r.y + 2, r.width - 4, r.height - 4}, ui::TONE_25, fb);

    char buf[32];
    snprintf(buf, sizeof(buf), "%s", value);
    // impact_128 holds about three glyphs, so a hero speed keeps its decimal
    // only below 10 — "round numbers until the churn stops".
    if (isSpeed) {
        double v = units::speed(s.speedKmh, s.useMiles);
        snprintf(buf, sizeof(buf), v < 10.0 ? "%.1f" : "%.0f", v);
        unit = "";
    }

    const int cx = r.x + r.width / 2;
    ui::label(cx, r.y + ui::CELL_PAD + Arial_B.ascender, labelStr, fb, ui::INK,
              &Arial_B);

    const int innerW = r.width - 2 * ui::CELL_PAD;
    const int unitW = (unit && unit[0]) ? ui::textWidth(&Arial_B, unit) + 10 : 0;
    // Largest face that fits the hero cell in both directions. Our hero cell is
    // taller than the drop's hero region (the layout is rider-configurable), so
    // impact_128 alone left it looking half-empty — step up when there is room.
    const int barH = zoneBar ? 18 + ui::STEP : 0;
    const int top = r.y + ui::CELL_PAD + Arial_B.ascender + ui::HALF_STEP;
    const int bot = r.y + r.height - ui::CELL_PAD - barH;
    static const EpdFont* const heroLadder[] = {&Impact_XL, &Impact_H, &Impact_M,
                                                &Impact_V};
    const EpdFont* vf = &Impact_V;
    for (const EpdFont* f : heroLadder) {
        if (ui::textWidth(f, buf) + unitW <= innerW &&
            epdc_digit_height(f) <= bot - top) {
            vf = f; break;
        }
    }

    const int vh = epdc_digit_height(vf);
    const int baseline = top + ((bot - top) + vh) / 2;

    const int vw = ui::textWidth(vf, buf);
    const int startX = r.x + (r.width - vw - unitW) / 2;
    ui::text(vf, startX, baseline, buf, fb, EPD_DRAW_ALIGN_LEFT, ui::INK);
    if (unitW)
        ui::text(&Arial_B, startX + vw + 10, baseline, unit, fb,
                 EPD_DRAW_ALIGN_LEFT, ui::INK);
    if (zoneBar) drawPowerZoneBar(s, r.y + r.height - ui::CELL_PAD - 18, fb);
}

// The WIDEST string a field can ever produce, used to choose its type size.
//
// Sizing from the live value made the type resize mid-ride: distance goes
// "0.0" -> "10.5" and ride time rolls past an hour, so the class stepped a face
// and every cell in it visibly grew or shrank. On e-paper that is also a full
// repaint of the row for no new information. Size for the worst case once and
// the number stays put for the whole ride.
const char* dashSizingHint(uint8_t field) {
    switch (field) {
        case DF_SPEED:       return "88.8";
        case DF_POWER3S:
        case DF_POWER:       return "888";
        case DF_HEART_RATE:
        case DF_CADENCE:     return "888";
        case DF_DISTANCE:
        case DF_ROUTE_LEFT:  return "888.8";
        case DF_RIDE_TIME:
        case DF_MOVING_TIME: return "88:88:88";
        case DF_CLIMB:
        case DF_ALTITUDE:    return "8888";
        case DF_GRADE:       return "88.8";
        case DF_BATTERY:     return "888";
        case DF_SATELLITES:  return "88";
        case DF_CLOCK:       return "88:88";
        default:             return "888";
    }
}

// One packed row: the items sharing it, and the height share it asked for.
struct DashRow { int first, count, weight; };

// Is there a source for this field right now?
//
// Only PAIRING-level state counts. A dashboard that reflows every time a value
// goes momentarily invalid would be unreadable — and on e-paper each reflow is
// a full repaint — so this asks "is the strap paired?", not "did a packet
// arrive this second". Grade and altitude are deliberately NOT here even though
// they can read "--": their source is the map DEM, whose validity flips as you
// ride in and out of downloaded coverage, and a field that vanishes at a tile
// boundary would be worse than one showing a dash.
bool dashFieldAvailable(uint8_t field, const RideState& s) {
    switch (field) {
        case DF_POWER3S:
        case DF_POWER:       return s.powerConnected;
        case DF_HEART_RATE:  return s.hrConnected;
        // Just the flag now: cadence from a power meter's crank data sets it
        // too. The old "or powerConnected" guessed that any meter implies
        // cadence, which showed an empty CADENCE cell for meters that send no
        // crank revolutions.
        case DF_CADENCE:     return s.cadenceConnected;
        case DF_ROUTE_LEFT:  return s.routeActive;
        default:             return true;
    }
}

// A dashboard cell that adapts to the height it is given.
//
// cell() above cannot: its label, value and unit sit at FIXED offsets tuned for
// the old 256 px grid, so in a short cell the Impact_40 value grows straight
// through both. A configurable layout hands out cells from ~90 px (a row of
// `small` fields) to ~300 px, so the type has to be chosen for the box, and the
// caption and unit dropped when there is genuinely no room rather than
// overlapped. Measured cap heights, matching the constants cell() already uses.
void dashCell(int x0, int y0, int x1, int y1, const char* labelStr,
              const char* value, const char* unit, uint8_t* fb) {
    const int cx = (x0 + x1) / 2;

    // Bound the content band before laying anything out.
    //
    // The caption pins to the top of the cell and the unit to the bottom, which
    // reads correctly at anything near the natural size. It falls apart when a
    // cell is enormous: with sensors unpaired their fields are dropped and the
    // survivors can inherit the whole panel, which left a value floating in the
    // middle with its unit stranded 400 px beneath it. So a very tall cell gets
    // a natural-height band centred inside it, and the surrounding space is
    // simply left empty — deliberate air rather than stretched furniture.
    constexpr int kMaxBand = 260;
    if (y1 - y0 > kMaxBand) {
        const int mid = (y0 + y1) / 2;
        y0 = mid - kMaxBand / 2;
        y1 = mid + kMaxBand / 2;
    }
    const int h = y1 - y0;

    // Caption and unit bands, INCLUDING the air around them, scaled to the cell.
    //
    // Two failure modes to sit between. Reserving only the glyphs (the first
    // pass) drew the caption at a fixed y0+24 and the unit at y1-8, welding both
    // to the cell's rules. Reserving a fixed generous band instead then starved
    // short cells: a `small` row could no longer afford a caption at all, and a
    // battery reading "76" with no label above it says nothing.
    //
    // So the air is a fraction of the cell, clamped: tall cells get room to
    // breathe, short ones keep their caption by giving up padding rather than
    // meaning.
    const int pad = h / 12 < 6 ? 6 : (h / 12 > 14 ? 14 : h / 12);

    // Usable width. Everything below has to fit inside this, not just inside
    // the height — a long value ("1:47:12") or a long caption ("MOVING TIME")
    // in a half-width cell overflows sideways at a size the height alone would
    // happily allow, and nothing was checking for it.
    const int availW = (x1 - x0) - 20;

    auto lineH = [](const EpdFont* f) { return f->ascender - f->descender; };

    // Caption: step up to the larger face when the cell can afford it both
    // ways. A big cell with a 14 pt caption looks unfinished next to its
    // numerals.
    const EpdFont* labelFont = &Arial_L;
    if (h >= 200 && ui::labelWidth(&Arial_B, labelStr) <= availW &&
        Arial_B.ascender + pad + 40 < h) {
        labelFont = &Arial_B;
    }

    const int kLabelH = pad + labelFont->ascender;
    const int kUnitH = pad + 20;     // unit glyphs + air
    const int kLabelBase = pad + labelFont->ascender;
    const int kUnitGap = pad;        // unit baseline, up from the cell bottom

    // Drop the least important thing first when space runs out: the unit
    // (usually inferable — BPM under a heart rate), then the caption.
    const int floorH = Arial_L.ascender;
    bool showLabel = h >= kLabelH + floorH + 4;
    bool showUnit = unit && unit[0] && h >= kLabelH + floorH + kUnitH + 4;

    int top = y0 + (showLabel ? kLabelH : 0);
    int bottom = y1 - (showUnit ? kUnitH : 0);
    int valueH = bottom - top;

    // Largest face that fits the box in BOTH directions. Bitmap fonts, so this
    // is a ladder rather than a scale factor — but it is the difference between
    // a value sized for its cell and one that either wastes half of it or runs
    // out the side.
    // Largest face whose DIGITS fit the box, in both directions.
    //
    // Height is measured with epdc_digit_height (the '0' glyph's bitmap), not
    // the font's line box and not epd_get_string_rect's height — both include
    // the full ascender-to-descender band, which for Impact_40 is 103 px around
    // 58 px of actual digits. Testing against the band rejected the big face in
    // every cell under ~180 px and quietly dropped half the dashboard to 20 pt.
    // Width matters just as much: "1:47:12" at Impact_40 is 214 px and a
    // half-width cell has 250 px, so a longer value must step down or run out
    // the side — nothing was checking that at all before.
    static const EpdFont* const kLadder[] = {&Impact_V, &Impact_T, &Arial_B,
                                            &Arial_L};
    const EpdFont* valueFont = &Arial_L;
    int vH = epdc_digit_height(&Arial_L);
    for (const EpdFont* f : kLadder) {
        const int ih = epdc_digit_height(f);
        if (ih + 4 <= valueH && ui::textWidth(f, value) <= availW) {
            valueFont = f;
            vH = ih;
            break;
        }
    }

    if (showLabel) ui::label(cx, y0 + kLabelBase, labelStr, fb, 0x00, labelFont);
    // Centre the ink box. Digits (and ':' and '.') have no descender, so the
    // bottom of that box IS the baseline ui::text() wants.
    int baseline = top + (valueH + vH) / 2;
    if (baseline > bottom) baseline = bottom;
    ui::text(valueFont, cx, baseline, value, fb, EPD_DRAW_ALIGN_CENTER, 0x00);
    if (showUnit)
        ui::text(&Arial_L, cx, y1 - kUnitGap, unit, fb, EPD_DRAW_ALIGN_CENTER, 0x00);
}

// Set while laying the fields out, read by ui_render_dashboard_toned(). A
// plain file-static because only the UI task ever renders.
bool g_dashToned = false;

void ui_render_dashboard(const RideState& s, bool navActive,
                         const DashLayout& layout, uint8_t* fb) {
    const int W = epd_rotated_display_width();
    const int H = epd_rotated_display_height();
    char buf[32];
    g_dashToned = false;

    ui::statusBar(s, fb);

    // The turn banner owns the 138 px under the status bar while navigating, so
    // the fields pack into whatever is left rather than being drawn over it.
    const int top = ui::STATUS_H + (navActive ? 138 : 0);

    const DashLayout& src = layout.count > 0 ? layout : dashDefaultLayout();

    // --- Drop fields with nothing behind them ----------------------------
    // A configured field whose sensor is not paired is REMOVED, and what is
    // left expands to fill the panel; it comes back, in its configured place,
    // the moment the sensor connects.
    //
    // The alternative shipped briefly and was wrong: a hero POWER cell with no
    // power meter quietly rendered SPEED instead. It kept the panel full, but a
    // number that changes identity is a worse lie than a missing one — the
    // caption is the only clue, and nobody reads the caption on a number they
    // check at 30 km/h.
    DashLayout L;
    for (int i = 0; i < src.count; ++i)
        if (s.showOffline || dashFieldAvailable(src.items[i].field, s))
            L.items[L.count++] = src.items[i];

    // Everything configured is unavailable — a fresh device with a power-only
    // layout and no sensors yet. Speed always has a source, so the panel shows
    // something rather than going blank.
    if (L.count == 0) {
        L.items[0] = {DF_SPEED, DZ_HERO, false};
        L.count = 1;
    }

    // --- Pack items into rows -------------------------------------------
    // A `half` item pairs with the NEXT item if that one is also `half`;
    // otherwise it takes the full width on its own. Note this runs AFTER the
    // filter, so a pair whose partner dropped out becomes a full-width row on
    // its own — which is the reflow the rider sees when a strap disconnects.
    // Two per row is the limit: three columns of Impact digits on a 540 px
    // panel is unreadable, and silently shrinking the font would break the
    // alignment between rows that makes the grid scannable.
    DashRow rows[DASH_MAX_ITEMS];
    int rowCount = 0;
    // Height share per size. Roughly the proportions of the original design:
    // the hero was ~3x a grid cell.
    static const int kWeight[DZ_COUNT] = {2, 3, 4, 8};
    for (int i = 0; i < L.count && rowCount < DASH_MAX_ITEMS; ) {
        int n = 1;
        if (L.items[i].half && i + 1 < L.count && L.items[i + 1].half) n = 2;
        int w = kWeight[L.items[i].size < DZ_COUNT ? L.items[i].size : DZ_MEDIUM];
        if (n == 2) {
            int w2 = kWeight[L.items[i + 1].size < DZ_COUNT ? L.items[i + 1].size
                                                            : DZ_MEDIUM];
            if (w2 > w) w = w2;   // a row is as tall as its tallest member
        }
        rows[rowCount++] = {i, n, w};
        i += n;
    }
    if (rowCount == 0) return;

    int totalWeight = 0;
    for (int r = 0; r < rowCount; ++r) totalWeight += rows[r].weight;

    // --- Lay out, then pick one face per size class, then draw --------------
    // Two passes on purpose. A cell that fits its own value independently makes
    // same-sized cells disagree: "1:47:12" is wider than "54.8", so it stepped
    // down a face and rendered visibly smaller in an identical box. The size a
    // rider CONFIGURED has to be the size they see, so every cell of a given
    // size class uses the smallest face any of them needs.
    struct Placed {
        EpdRect r;
        uint8_t field, size;
        bool hero, stale;
        char value[32];
        const char* unit;
    } placed[DASH_MAX_ITEMS];
    int placedN = 0;

    const int gutters = (rowCount - 1) * ui::GUTTER;
    const int availH = (H - top - ui::MARGIN) - gutters;
    int y = top + ui::MARGIN - ui::STEP;
    for (int r = 0; r < rowCount; ++r) {
        int rowH = (r == rowCount - 1) ? (H - ui::MARGIN - y)
                                       : availH * rows[r].weight / totalWeight;
        const DashRow& row = rows[r];
        const int halfW = (ui::CONTENT_W - ui::GUTTER) / 2;
        for (int c = 0; c < row.count; ++c) {
            const DashItem& it = L.items[row.first + c];
            Placed& p = placed[placedN++];
            p.r.y = y;
            p.r.height = rowH;
            if (row.count == 2) {
                p.r.x = c == 0 ? ui::CONTENT_X : ui::CONTENT_X + halfW + ui::GUTTER;
                p.r.width = halfW;
            } else {
                p.r.x = ui::CONTENT_X;
                p.r.width = ui::CONTENT_W;
            }
            p.field = it.field;
            p.size = it.size;
            p.hero = (it.size == DZ_HERO && row.count == 1 && rowH >= 200);
            p.unit = "";
            dashFieldValue(p.field, s, p.value, sizeof(p.value), &p.unit);
            // Grey on the SENSOR, not on the text. Keying off a "--" value meant
            // a power meter that dropped out while its last reading lingered in
            // the shared state kept rendering a live-looking number. The field
            // is inactive the moment its source is gone, whatever it last said.
            p.stale = !dashFieldAvailable(p.field, s) ||
                      (p.value[0] == '-' && p.value[1] == '-');
            if (p.stale) g_dashToned = true;
        }
        y += rowH + ui::GUTTER;
    }

    // Smallest face any cell of each (size class, width) needs.
    //
    // Keyed on WIDTH as well as size: a full-width cell and a half-width cell
    // are not "fields of a similar size", and lumping them together let a long
    // value in a narrow box ("0:00:00" in 240 px) hold down a full-width SPEED
    // cell with twice the room — which is how the biggest cell on the panel
    // ended up with the smallest type.
    int classIdx[DZ_COUNT][2];
    for (int i = 0; i < DZ_COUNT; ++i) classIdx[i][0] = classIdx[i][1] = 0;
    // ONE caption size for the whole screen. Values scale with their cell —
    // that is the hierarchy the rider configured — but captions are chrome, and
    // a "SPEED" larger than "DISTANCE" reads as emphasis nobody asked for.
    int labelIdx = 0;
    for (int i = 0; i < placedN; ++i) {
        const Placed& p = placed[i];
        if (p.hero) continue;   // the hero has its own ladder
        const int availW = p.r.width - 2 * ui::CELL_PAD;
        const int unitW = (p.unit && p.unit[0])
                              ? ui::textWidth(&Arial_B, p.unit) + 6 : 0;
        const int valH = p.r.height - ui::CELL_PAD * 2 - Arial_B.ascender
                         - ui::HALF_STEP;
        const int idx = ui::valueFontIndex(ui::kValueLadder,
                                           dashSizingHint(p.field), availW,
                                           valH, unitW);
        const int wb = (p.r.width > ui::CONTENT_W * 3 / 4) ? 1 : 0;
        if (idx > classIdx[p.size][wb]) classIdx[p.size][wb] = idx;
        // Captions equalise the same way: "GRADE" fitted the 20 pt face while
        // "ALTITUDE" did not, so two cells in one row wore different captions.
        int li = ui::LABEL_LADDER_N - 1;
        for (int k = 0; k < ui::LABEL_LADDER_N; ++k)
            if (ui::labelWidth(ui::kLabelLadder[k], dashFieldLabel(p.field))
                <= availW) { li = k; break; }
        if (li > labelIdx) labelIdx = li;
    }

    for (int i = 0; i < placedN; ++i) {
        const Placed& p = placed[i];
        if (p.hero) {
            heroCell(p.r, dashFieldLabel(p.field), p.value, p.unit, s,
                     isPowerField(p.field), p.field == DF_SPEED, p.stale, fb);
        } else {
            const int wb = (p.r.width > ui::CONTENT_W * 3 / 4) ? 1 : 0;
            ui::cell(p.r, dashFieldLabel(p.field), p.value, p.unit, fb, p.stale,
                     ui::kValueLadder[classIdx[p.size][wb]],
                     ui::kLabelLadder[labelIdx]);
        }
    }
}

bool ui_render_dashboard_toned() { return g_dashToned; }

// Defined below with the list helpers; declared here so the sheet can clamp
// its body lines to the content column.
static void fitText(const EpdFont* font, const char* str, int maxW,
                    char* out, size_t outSize);

// Primary buttons are solid, secondary are outlined; one primary per screen.
static void sheetButton(const EpdRect& r, const char* text, bool primary,
                        uint8_t* fb) {
    if (primary) {
        epd_fill_rect(r, ui::INK, fb);
    } else {
        for (int e = 0; e < 3; ++e)   // specimen: secondary border is 3 px
            epd_draw_rect({r.x + e, r.y + e, r.width - 2 * e, r.height - 2 * e},
                          ui::INK, fb);
    }
    // Specimen sets button text in impact_40; step down only if it will not fit.
    const EpdFont* f = ui::textWidth(&Impact_T, text) <= r.width - 16
                           ? &Impact_T : &Impact_T;
    ui::text(f, r.x + r.width / 2, r.y + r.height / 2 + epdc_digit_height(f) / 2,
             text, fb, EPD_DRAW_ALIGN_CENTER, primary ? ui::PAPER : ui::INK);
}

void ui_render_summary(const RideSummary& r, uint8_t* fb) {
    const int W = epd_rotated_display_width();
    const int H = epd_rotated_display_height();
    char buf[64];

    // SHEET — a MODAL over whatever screen is behind it, not a page of its own.
    // Specimen: the screen behind is scrimmed with a 2 px checker, then a
    // 520 px panel is anchored to the bottom with a 6 px top rule and 40/24/24
    // padding. The scrim is a 1-bit tone, never a grey: this sits over a live
    // screen and a mid grey would ghost.
    const int sheetY = H - 520;
    ui::fillTone({0, 0, W, sheetY}, ui::TONE_50, fb);
    epd_fill_rect({0, sheetY, W, H - sheetY}, ui::PAPER, fb);
    epd_fill_rect({0, sheetY, W, 6}, ui::INK, fb);

    const int x = ui::MARGIN;                 // sheet padding: 24 sides
    int y = sheetY + 6 + 40;                  // 6 px rule + 40 px top padding

    // Tracked label, then the hero figure with its unit set INLINE in impact —
    // the specimen reads "54.8 KM" as one line, not a value with a superscript.
    const int lw = ui::labelWidth(&Arial_L, "END OF RIDE");
    ui::label(x + lw / 2, y + Arial_L.ascender, "END OF RIDE", fb);
    y += Arial_L.ascender + 8;

    snprintf(buf, sizeof(buf), "%.1f %s", units::distM(r.distanceM, r.useMiles),
             units::distLabel(r.useMiles));
    const int heroH = epdc_digit_height(&Impact_M);
    ui::text(&Impact_M, x, y + heroH, buf, fb, EPD_DRAW_ALIGN_LEFT, ui::INK);
    y += heroH + 12;

    char moving[16];
    snprintf(moving, sizeof(moving), "%lu:%02lu",
             (unsigned long)(r.movingS / 3600),
             (unsigned long)((r.movingS / 60) % 60));
    if (r.avgPowerW > 0)
        snprintf(buf, sizeof(buf), "%s moving · %.0f %s ascent · %d W avg", moving,
                 units::elev(r.climbedM, r.useMiles), r.useMiles ? "ft" : "m",
                 r.avgPowerW);
    else
        snprintf(buf, sizeof(buf), "%s moving · %.0f %s ascent · %d bpm", moving,
                 units::elev(r.climbedM, r.useMiles), r.useMiles ? "ft" : "m",
                 r.avgHrBpm);
    char line[72];
    fitText(&Arial_B, buf, W - 2 * ui::MARGIN, line, sizeof(line));
    ui::text(&Arial_B, x, y + Arial_B.ascender, line, fb, EPD_DRAW_ALIGN_LEFT,
             ui::INK);

    sheetButton(kSaveButton, "SAVE", true, fb);
    sheetButton(kDiscardButton, "DISCARD", false, fb);
    sheetButton(kResumeButton, "RESUME", false, fb);
}

void ui_render_update_overlay(const char* phase, int pct, uint8_t* fb) {
    const int W = epd_rotated_display_width();
    const int H = epd_rotated_display_height();

    // Centered modal card.
    const int bw = W - 56, bh = 340;
    const int bx = (W - bw) / 2, by = (H - bh) / 2;
    const int innerW = bw - 16;   // text must stay inside this (with margin)
    epd_fill_rect({bx, by, bw, bh}, 0xFF, fb);
    for (int i = 0; i < 4; ++i) epd_draw_rect({bx + i, by + i, bw - 2 * i, bh - 2 * i}, 0x00, fb);

    // Pick the largest font that keeps a centered string inside the card.
    auto fitFont = [&](const char* s) -> const EpdFont* {
        return ui::textWidth(&Arial_B, s) <= innerW ? &Arial_B : &Arial_L;
    };

    // Inverted title band
    epd_fill_rect({bx, by, bw, 76}, 0x00, fb);
    ui::text(fitFont("UPDATING FIRMWARE"), W / 2, by + 50, "UPDATING FIRMWARE", fb,
             EPD_DRAW_ALIGN_CENTER, 0xFF);

    char sub[48];
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    snprintf(sub, sizeof(sub), "%s  %d%%", phase, pct);
    ui::text(fitFont(sub), W / 2, by + 138, sub, fb, EPD_DRAW_ALIGN_CENTER, 0x00);

    // Progress bar
    const int pbx = bx + 40, pby = by + 170, pbw = bw - 80, pbh = 44;
    epd_draw_rect({pbx, pby, pbw, pbh}, 0x00, fb);
    epd_draw_rect({pbx + 1, pby + 1, pbw - 2, pbh - 2}, 0x00, fb);
    int fillw = (pbw - 8) * pct / 100;
    if (fillw > 0) epd_fill_rect({pbx + 4, pby + 4, fillw, pbh - 8}, 0x00, fb);

    ui::text(&Arial_L, W / 2, by + 270, "Keep the app open and the", fb,
             EPD_DRAW_ALIGN_CENTER, 0x00);
    ui::text(&Arial_L, W / 2, by + 300, "device nearby until it finishes.", fb,
             EPD_DRAW_ALIGN_CENTER, 0x00);
}

// Copy `str` into `out`, truncating with a trailing ".." if it would exceed
// `maxW` px in `font`. UTF-8 safe (never cuts mid-character). Use for any
// variable-length text (sensor/route names, turn instructions) so it can never
// run past its bounds.
static void fitText(const EpdFont* font, const char* str, int maxW,
                    char* out, size_t outSize) {
    if (ui::textWidth(font, str) <= maxW || outSize < 4) {
        snprintf(out, outSize, "%s", str);
        return;
    }
    size_t n = strlen(str);
    while (n > 1) {
        n--;
        while (n > 0 && ((unsigned char)str[n] & 0xC0) == 0x80) n--;  // UTF-8 boundary
        snprintf(out, outSize, "%.*s..", (int)n, str);
        if (ui::textWidth(font, out) <= maxW) return;
    }
    snprintf(out, outSize, "..");
}

// LIST ROW internals, straight off the specimen: 492 x 148, padding 24/16, the
// title/subtitle block vertically centred, and a solid right-pointing triangle
// (20 wide, 28 tall) as the affordance. The block is centred as a unit — the
// specimen uses align-items:center on a flex row, not fixed baselines.
static void drawListRowText(int x, int rowY, const char* title, const char* sub,
                            uint8_t fg, uint8_t* fb) {
    // "No lowercase in impact. Ever." — the face carries no lowercase glyphs at
    // all, so a title like "Start Ride" silently drew as "S R". Uppercasing here
    // rather than at each call site means no caller can reintroduce it, and the
    // titles come from data (sensor names, route filenames) that we do not own.
    char up[48];
    size_t n = 0;
    for (const char* p = title; *p && n < sizeof(up) - 1; ++p, ++n)
        up[n] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
    up[n] = 0;
    title = up;

    const int titleH = epdc_digit_height(&Impact_T);
    const int gap = 8;
    const int subH = Arial_B.ascender;
    const int blockH = titleH + gap + subH;
    const int top = rowY + (ui::ROW_H - blockH) / 2;
    ui::text(&Impact_T, x, top + titleH, title, fb, EPD_DRAW_ALIGN_LEFT, fg);
    ui::text(&Arial_B, x, top + titleH + gap + subH, sub, fb,
             EPD_DRAW_ALIGN_LEFT, fg);
}

static void drawRowMarker(int rowY, uint8_t fg, uint8_t* fb) {
    const int x = ui::CONTENT_X + ui::CONTENT_W - ui::CELL_PAD - 20;
    const int cy = rowY + ui::ROW_H / 2;
    epd_fill_triangle(x, cy - 14, x, cy + 14, x + 20, cy, fg, fb);
}

void ui_render_menu(const MenuInfo& m, uint8_t* fb) {
    const int W = epd_rotated_display_width();
    const int H = epd_rotated_display_height();
    char sub[64];

    // Header: MENU left (inset to match the rows), battery percent right
    // No rule here: kMenuRowTop sits on the status band, whose own 3 px rule
    // is already the separator. Drawing one too made two lines with a gap.

    struct Row {
        const char* title;
        const char* subtitle;
        bool inverted;
    };

    char startSub[64], sensorSub[64], historySub[64], settingsSub[64];
    if (m.recording) {
        snprintf(startSub, sizeof(startSub), "recording · %.1f %s",
                 units::distM(m.rideDistanceM, m.useMiles),
                 m.useMiles ? "mi" : "km");
    } else {
        int n = (m.hr ? 1 : 0) + (m.pwr ? 1 : 0) + (m.cad ? 1 : 0);
        snprintf(startSub, sizeof(startSub), "%s · %d sensor%s connected",
                 m.gpsReady ? "GPS ready" : "waiting for GPS", n,
                 n == 1 ? "" : "s");
    }
    snprintf(sensorSub, sizeof(sensorSub), "HR %s · Power %s · Cadence %s",
             m.hr ? "OK" : "--", m.pwr ? "OK" : "--", m.cad ? "OK" : "--");
    if (m.sdOk) snprintf(historySub, sizeof(historySub), "%d ride%s on card",
                         m.rideCount, m.rideCount == 1 ? "" : "s");
    else snprintf(historySub, sizeof(historySub), "no SD card");
    snprintf(settingsSub, sizeof(settingsSub), "FTP · timezone");

    const Row rows[kMenuRowCount] = {
        {m.recording ? "Stop Ride" : "Start Ride", startSub, true},
        {"Navigate", m.routeLine, false},
        {"Sensors", sensorSub, false},
        {"Ride History", historySub, false},
        {"Settings", settingsSub, false},
    };

    // Same LIST row recipe as ui_render_list: 492 x 148 inset on the margin,
    // condensed_28 title over a sentence-case subtitle, 2 px rule below, and a
    // full invert for the selected row.
    const int tx = ui::CONTENT_X + ui::CELL_PAD;
    // Reserve the row marker: the specimen's 20 px triangle plus a 16 px gap.
    const int textW = ui::CONTENT_W - 2 * ui::CELL_PAD - 36;
    for (int i = 0; i < kMenuRowCount; ++i) {
        int y = kMenuRowTop + i * ui::ROW_H;
        uint8_t fg = ui::INK;
        if (rows[i].inverted) {
            // Full-bleed: a selected row is dark all the way across.
            epd_fill_rect({0, y, W, ui::ROW_H}, ui::INK, fb);
            fg = ui::PAPER;
        }
        char mt[48], ms[64];
        fitText(&Impact_T, rows[i].title, textW, mt, sizeof(mt));
        fitText(&Arial_B, rows[i].subtitle, textW, ms, sizeof(ms));
        drawListRowText(tx, y, mt, ms, fg, fb);
        drawRowMarker(y, fg, fb);
        if (!rows[i].inverted)
            epd_fill_rect({0, y + ui::ROW_H - ui::RULE, W, ui::RULE}, ui::INK, fb);
    }

    // Footer status line
    if (m.sdOk) {
        snprintf(sub, sizeof(sub), FIRMWARE_VERSION " · %lu MB free · %s",
                 (unsigned long)m.sdFreeMB, m.gpsReady ? "GPS lock" : "no GPS");
    } else {
        snprintf(sub, sizeof(sub), FIRMWARE_VERSION " · no SD card · %s",
                 m.gpsReady ? "GPS lock" : "no GPS");
    }
    ui::text(&Arial_L, W / 2, H - 24, sub, fb, EPD_DRAW_ALIGN_CENTER, 0x00);
}

// A turn arrow read from the instruction text: left, right, straight or
// U-turn. Drawn as a shaft plus a solid head — no glyph exists for these, and
// the system prefers a drawn mark to an icon font.
static void drawTurnArrow(int cx, int cy, const char* instruction, uint8_t fg,
                          uint8_t* fb) {
    bool left = false, right = false, uturn = false;
    for (const char* p = instruction; *p; ++p) {
        if (strncasecmp(p, "left", 4) == 0) left = true;
        if (strncasecmp(p, "right", 5) == 0) right = true;
        if (strncasecmp(p, "u-turn", 6) == 0 || strncasecmp(p, "uturn", 5) == 0)
            uturn = true;
    }
    const int t = 6;

    // Centre the SHAPE, not the shaft. A right turn draws its elbow and head
    // entirely to one side, so anchoring on the shaft left the glyph hard
    // against the text with a column of empty space on the other side — every
    // arrow appeared to hang at a different offset.
    if (left || right) {
        const int dir = left ? -1 : 1;
        const int elbow = 20, head = 14;
        // Extent runs from the shaft edge to the head tip on the turn side.
        const int lo = dir < 0 ? -(elbow + head) : -t / 2;
        const int hi = dir < 0 ? t / 2 : (elbow + head);
        const int ox = cx - (lo + hi) / 2;
        epd_fill_rect({ox - t / 2, cy - 4, t, 22}, fg, fb);              // shaft
        epd_fill_rect({dir < 0 ? ox - elbow : ox, cy - 4, elbow, t}, fg, fb);
        const int hx = ox + dir * elbow;
        epd_fill_triangle(hx, cy - 16, hx, cy + 12, hx + dir * head, cy - 2, fg, fb);
    } else if (uturn) {
        const int lo = -18, hi = 18;
        const int ox = cx - (lo + hi) / 2;
        epd_fill_rect({ox - 12, cy - 18, t, 30}, fg, fb);
        epd_fill_rect({ox - 12, cy - 18, 24, t}, fg, fb);
        epd_fill_rect({ox + 12 - t, cy - 18, t, 18}, fg, fb);
        epd_fill_triangle(ox + 12 - t - 8, cy, ox + 12 + 8, cy, ox + 12 - t / 2,
                          cy + 16, fg, fb);
    } else {
        epd_fill_rect({cx - t / 2, cy - 2, t, 22}, fg, fb);
        epd_fill_triangle(cx - 12, cy - 2, cx + 12, cy - 2, cx, cy - 20, fg, fb);
    }
}

void ui_render_list(const char* title, const ListRow* rows, int count,
                    const char* footer, uint8_t* fb, bool turnArrows) {
    const int W = epd_rotated_display_width();

    // LIST template: title in the status band, rows on the 24 px margin, one
    // 96 px BACK strip as the single exit. Rows are 148 px with a 2 px rule
    // below — the rule is chrome and is drawn once, never inside a fast region.
    // No rule here: kMenuRowTop sits on the status band, whose own 3 px rule
    // is already the separator. Drawing one too made two lines with a gap.

    const int textW = ui::CONTENT_W - 2 * ui::CELL_PAD - (turnArrows ? 64 : 36);
    for (int i = 0; i < count && i < kMenuRowCount; ++i) {
        int y = kMenuRowTop + i * ui::ROW_H;
        uint8_t fg = ui::INK;
        if (rows[i].inverted) {
            // Selected state is a full invert of the rectangle — the system's
            // one way to show state, and the only one a DU pass renders cleanly.
            // Full-bleed: a selected row is dark all the way across.
            epd_fill_rect({0, y, W, ui::ROW_H}, ui::INK, fb);
            fg = ui::PAPER;
        }
        char t[40], sub[64];
        fitText(&Impact_T, rows[i].title, textW, t, sizeof(t));
        fitText(&Arial_B, rows[i].subtitle, textW, sub, sizeof(sub));
        // Title on the row's own 24 px inset, subtitle a step below it. Two
        // faces, one voice each: condensed_28 titles, sentence-case body.
        // Text sits on the row's own padding, so an inverted row keeps an even
        // black border around its content instead of bleeding to the panel edge.
        // With arrows the text starts clear of them, and the right-edge marker
        // goes away — a row that already shows its turn does not need a second
        // affordance pointing at nothing.
        const int tx = ui::CONTENT_X + ui::CELL_PAD + (turnArrows ? 64 : 0);
        drawListRowText(tx, y, t, sub, fg, fb);
        if (turnArrows)
            drawTurnArrow(ui::CONTENT_X + ui::CELL_PAD + 24, y + ui::ROW_H / 2,
                          rows[i].title, fg, fb);
        else
            drawRowMarker(y, fg, fb);
        if (!rows[i].inverted)
            epd_fill_rect({0, y + ui::ROW_H - ui::RULE, W, ui::RULE}, ui::INK, fb);
    }
    if (count == 0) {
        // Say what is absent, never leave the field empty.
        ui::text(&Arial_B, W / 2, kMenuRowTop + 80, "Nothing found", fb,
                 EPD_DRAW_ALIGN_CENTER, ui::INK);
    }

    if (footer && footer[0]) {
        ui::text(&Arial_L, W / 2, kContentBottom, footer, fb,
                 EPD_DRAW_ALIGN_CENTER, ui::INK);
    }
}

// iOS-style pill switch: black filled pill with a white knob on the right
// when on; white outlined pill with a black knob on the left when off.
// SWITCH — drawn 120 x 52, no radii (the system forbids them, and the driver
// has no antialiasing to make a curve read as one anyway). State is the whole
// rectangle inverted, which is the one state cue a DU pass renders cleanly.
// The hit rect is 120 x 88; only the drawn shape is this size.
// SWITCH — 120 x 52, border 2, split into two halves; the ACTIVE half is filled
// and carries its word in reverse. Not a single filled rectangle: the specimen
// shows position, so "off" is still a legible state rather than an empty box.
static void settingsToggle(int x, int y, int w, int h, bool on, uint8_t* fb,
                           const char* onText = "ON",
                           const char* offText = "OFF") {
    for (int b = 0; b < ui::RULE; ++b)
        epd_draw_rect({x + b, y + b, w - 2 * b, h - 2 * b}, ui::INK, fb);
    const int half = w / 2;
    const int fx = on ? x : x + half;      // ON fills the left half, OFF the right
    epd_fill_rect({fx + ui::RULE, y + ui::RULE, half - ui::RULE,
                   h - 2 * ui::RULE}, ui::INK, fb);
    // Only the filled half is labelled — the empty half is left blank, which is
    // what makes the switch read as a position rather than two competing words.
    ui::label(fx + half / 2, y + h / 2 + 6, on ? onText : offText, fb, ui::PAPER);
}

void ui_render_settings(const SettingsInfo& si, uint8_t* fb) {
    const int W = epd_rotated_display_width();

    // No rule here: kMenuRowTop sits on the status band, whose own 3 px rule
    // is already the separator. Drawing one too made two lines with a gap.

    struct Row {
        const char* label;
        char value[16];
        bool toggle;   // switch instead of a +/- stepper
        bool on;
    } rows[kSettingsRowCount];
    snprintf(rows[0].value, sizeof(rows[0].value), "%d", si.ftpW);
    rows[0].label = "FTP";
    rows[0].toggle = false;
    int tzH = si.tzMin / 60, tzM = abs(si.tzMin % 60);
    snprintf(rows[1].value, sizeof(rows[1].value), "%+d:%02d", tzH, tzM);
    rows[1].label = "TIMEZONE";
    rows[1].toggle = false;
    // UPPERCASE: impact carries no lowercase glyphs, so "Med" drew as "M".
    static const char* BL[] = {"OFF", "LOW", "MED", "BRIGHT"};
    snprintf(rows[2].value, sizeof(rows[2].value), "%s",
             BL[si.backlight < 0 ? 0 : si.backlight > 3 ? 3 : si.backlight]);
    rows[2].label = "BACKLIGHT";
    rows[2].toggle = false;
    snprintf(rows[3].value, sizeof(rows[3].value), "%s", si.useMiles ? "Miles" : "Km");
    rows[3].label = "UNITS";
    rows[3].toggle = true;
    rows[3].on = si.useMiles;
    snprintf(rows[4].value, sizeof(rows[4].value), "%s", si.usbDrive ? "On" : "Off");
    rows[4].label = "USB DRIVE";
    rows[4].toggle = true;
    rows[4].on = si.usbDrive;
    // Keep unpaired sensor fields on the dashboard rather than dropping them.
    snprintf(rows[5].value, sizeof(rows[5].value), "%s", si.showOffline ? "On" : "Off");
    rows[5].label = "SHOW OFFLINE";
    rows[5].toggle = true;
    rows[5].on = si.showOffline;
    // The same switch the phone's Mesh tab drives, surfaced here so the radio can
    // be shut off without a phone — it is the one setting on this page that costs
    // battery continuously while it is on.
    snprintf(rows[6].value, sizeof(rows[6].value), "%s", si.meshOn ? "On" : "Off");
    rows[6].label = "MESH RADIO";
    rows[6].toggle = true;
    rows[6].on = si.meshOn;

    for (int i = 0; i < kSettingsRowCount; ++i) {
        const int y = kMenuRowTop + i * kSettingsRowH;
        const int midY = y + kSettingsRowH / 2;

        // Label: tracked uppercase arialbold_14, left-aligned on the content
        // column. label() centres, so anchor it at its own half width.
        // Specimen: the row label is arialbold_20 sentence-weight, left-aligned
        // on the 16 px padding — not a tracked 14 pt label.
        const int labelRoom = (rows[i].toggle ? settingsToggleX(i) : kSettingsMinusX)
                              - (ui::CONTENT_X + ui::CELL_PAD) - ui::HALF_STEP;
        char lab[24];
        fitText(&Arial_B, rows[i].label, labelRoom, lab, sizeof(lab));
        ui::text(&Arial_B, ui::CONTENT_X + ui::CELL_PAD, midY + 8, lab, fb,
                 EPD_DRAW_ALIGN_LEFT, ui::INK);

        if (rows[i].toggle) {
            // Switch only. The row previously showed BOTH a value ("Km") and a
            // switch reading OFF, which said the same thing twice and disagreed
            // about which way round it was. The system's switch component is a
            // label and the state, nothing else.
            // A switch reading ON/OFF beside "UNITS" says nothing — on what?
            // The two positions ARE the choice, so they carry the unit names.
            const bool isUnits = (i == kSettingsUnitsRow);
            settingsToggle(settingsToggleX(i), midY - kSettingsToggleH / 2,
                           settingsToggleW(i), kSettingsToggleH, rows[i].on, fb,
                           isUnits ? "MILES" : "ON", isUnits ? "KM" : "OFF");
        } else {
            // STEPPER: 88 x 88 targets, value centred in the 88 px between them.
            // The old layout drew the value from the row centre, so "250 W" and
            // "UTC-7" ran straight through both buttons.
            for (int b = 0; b < 2; ++b) {
                const int bx = b == 0 ? kSettingsMinusX : kSettingsPlusX;
                const EpdRect r = {bx, midY - kSettingsBtn / 2, kSettingsBtn,
                                   kSettingsBtn};
                for (int e = 0; e < ui::RULE; ++e)
                    epd_draw_rect({r.x + e, r.y + e, r.width - 2 * e,
                                   r.height - 2 * e}, ui::INK, fb);
                // impact_40 weight glyphs, drawn as bars so they match the
                // face's stroke rather than sitting thin inside an 88 px box.
                const int cx = r.x + r.width / 2, cy = r.y + r.height / 2;
                epd_fill_rect({cx - 18, cy - 3, 36, 7}, ui::INK, fb);
                if (b == 1) epd_fill_rect({cx - 3, cy - 18, 7, 36}, ui::INK, fb);
            }
            const int vx = kSettingsMinusX + kSettingsBtn + kSettingsGap
                           + kSettingsValueW / 2;
            const EpdFont* vf =
                ui::textWidth(&Impact_T, rows[i].value) <= kSettingsValueW
                    ? &Impact_T : &Impact_T;
            ui::text(vf, vx, midY + epdc_digit_height(vf) / 2, rows[i].value, fb,
                     EPD_DRAW_ALIGN_CENTER, ui::INK);
        }
        epd_fill_rect({0, y + kSettingsRowH - ui::RULE, W, ui::RULE}, ui::INK, fb);
    }

    // Navigation row, drawn with the LIST recipe so it reads as the same
    // component it is on every other screen — and kept to ONE dense row. With
    // the BACK strip gone there is 174 px left at the foot of the page, but
    // stretching the row to fill it made a single tappable line as tall as two
    // settings rows, with its text stranded at the top. The page just ends
    // instead; rows are a fixed height in this system.
    const int ny = kMenuRowTop + kSettingsGpsRow * kSettingsRowH;
    ui::text(&Impact_T, ui::CONTENT_X + ui::CELL_PAD, ny + 40, "GPS DEBUG", fb);
    ui::text(&Arial_B, ui::CONTENT_X + ui::CELL_PAD, ny + 72,
             "Receiver diagnostics", fb, EPD_DRAW_ALIGN_LEFT, ui::INK);
    epd_fill_rect({0, ny + kSettingsRowH - ui::RULE, W, ui::RULE}, ui::INK, fb);
}

void ui_render_gps_debug(const GpsDebugView& g, uint8_t* fb) {
    const int W = epd_rotated_display_width();

    // No rule here: kMenuRowTop sits on the status band, whose own 3 px rule
    // is already the separator. Drawing one too made two lines with a gap.

    struct Line {
        const char* label;
        char value[40];
    } lines[16];
    int n = 0;
#define DBG_LINE(lbl, ...)                                            \
    do {                                                              \
        lines[n].label = lbl;                                         \
        snprintf(lines[n].value, sizeof(lines[n].value), __VA_ARGS__); \
        n++;                                                          \
    } while (0)

    DBG_LINE("MODULE", "%s", g.moduleDetected ? g.module : "NOT DETECTED");
    DBG_LINE("NMEA BYTES", "%lu", (unsigned long)g.chars);
    DBG_LINE("CHECKSUM", "%lu / %lu", (unsigned long)g.passedCksum,
             (unsigned long)g.failedCksum);
    DBG_LINE("WITH FIX", "%lu", (unsigned long)g.withFix);
    DBG_LINE("SATS IN VIEW", "%d", g.satsInView);
    DBG_LINE("SATS IN USE", "%d", g.satsInUse);
    if (g.bestSnr > 0) DBG_LINE("BEST SIGNAL", "%d dB-Hz", g.bestSnr);
    else DBG_LINE("BEST SIGNAL", "--");
    if (g.hdop > 0) DBG_LINE("HDOP", "%.1f", g.hdop);
    else DBG_LINE("HDOP", "--");
    if (g.locValid) {
        DBG_LINE("FIX", "yes · age %lu ms", (unsigned long)g.locAgeMs);
        DBG_LINE("LAT / LON", "%.5f  %.5f", g.lat, g.lon);
        DBG_LINE("ALT / SPEED", "%.0f %s · %.1f %s",
                 units::elev(g.altM, g.useMiles), units::elevLabel(g.useMiles),
                 units::speed(g.speedKmh, g.useMiles), units::speedLabel(g.useMiles));
    } else {
        DBG_LINE("FIX", "NO FIX");
        DBG_LINE("LAT / LON", "--");
        DBG_LINE("ALT / SPEED", "--");
    }
    if (g.hour >= 0) DBG_LINE("UTC TIME", "%02d:%02d:%02d", g.hour, g.minute,
                              g.second);
    else DBG_LINE("UTC TIME", "--");
#undef DBG_LINE

    // This page repaints every second via the fast 1-bit DU refresh, which
    // snaps any gray to white — so every mark here must be pure black.
    // Spacing is derived so all rows fit between the header and the bottom.
    // Label left, value right, on the content column — the same left/right
    // cluster the status bar uses. Tracked uppercase labels, values in the body
    // face; rules inset to 492 like every other row on the device.
    const int y0 = kMenuRowTop + ui::STEP;
    const int step = n > 0 ? (kContentBottom - ui::STEP - y0) / n : 40;
    const int lx = ui::CONTENT_X + ui::CELL_PAD;
    const int rx = ui::CONTENT_X + ui::CONTENT_W - ui::CELL_PAD;
    for (int i = 0; i < n; ++i) {
        const int y = y0 + i * step + step - ui::STEP;
        // Fit the label to whatever the value leaves — a long label and a long
        // value previously overlapped in the middle of the row.
        const int room = rx - ui::textWidth(&Arial_B, lines[i].value)
                         - ui::STEP - lx;
        char lab[32];
        fitText(&Arial_L, lines[i].label, room, lab, sizeof(lab));
        ui::label(lx + ui::labelWidth(&Arial_L, lab) / 2, y, lab, fb);
        ui::text(&Arial_B, rx, y, lines[i].value, fb, EPD_DRAW_ALIGN_RIGHT,
                 ui::INK);
        if (i > 0) epd_fill_rect({0, y0 + i * step, W, 1}, ui::INK, fb);
    }
}

// Bottom sheet: ~360 px tall, drawn over the live screen behind it.
// SHEET geometry, shared with the end-of-ride modal: a 520 px panel anchored to
// the bottom with a 6 px top rule and 24 px side padding. The buttons sit on the
// same rows the summary uses, so all three modals are the same object.
const EpdRect kPowerSheet = {0, 440, 540, 520};
const EpdRect kPowerShutdown = {24, 700, 492, 96};
const EpdRect kPowerCancel = {24, 816, 492, 96};

// Draws the shared modal shell: scrim over the live screen, bottom panel, rule.
// The end-of-ride sheet, the power dialog and the navigation prompt are ONE
// component with different words in it.
// `scrim` tones what is behind the sheet, to push it back and say the modal has
// the input. Pass false when what is behind IS the thing being decided about —
// the route preview under the NAVIGATE prompt, where a 50% checker over fine map
// lines is exactly what stops the rider recognising the route they are being
// asked to accept.
static void sheetShell(uint8_t* fb, bool scrim = true) {
    const int W = epd_rotated_display_width();
    const int H = epd_rotated_display_height();
    const int y = H - 520;
    // Scrim the BODY only. Toning the status band as well made the clock and
    // battery unreadable behind the checker — that row is chrome the rider
    // still needs while a modal is up.
    if (scrim)
        fillTone({0, ui::STATUS_H, W, y - ui::STATUS_H}, ui::TONE_50, fb);
    epd_fill_rect({0, y, W, H - y}, ui::PAPER, fb);
    epd_fill_rect({0, y, W, 6}, ui::INK, fb);
}

// Title + hero line + one detail line, at the summary's sizes exactly.
static void sheetHead(const char* label, const char* hero, const char* detail,
                      uint8_t* fb) {
    const int W = epd_rotated_display_width();
    const int H = epd_rotated_display_height();
    const int x = ui::MARGIN;
    int y = (H - 520) + 6 + 40;

    const int lw = ui::labelWidth(&Arial_L, label);
    ui::label(x + lw / 2, y + Arial_L.ascender, label, fb);
    y += Arial_L.ascender + 8;

    // Impact carries no lowercase, no '_' and no '.' beyond its ASCII run, so a
    // route filename ("mission_dolores.gpx") drew as a row of blanks. Uppercase
    // it, turn separators into spaces and drop the extension — which is also
    // how a rider would read the name aloud.
    char clean[48];
    size_t n = 0;
    for (const char* p = hero; *p && n < sizeof(clean) - 1; ++p) {
        char c = *p;
        if (c == '.' && (strcasecmp(p, ".gpx") == 0)) break;
        if (c == '_' || c == '-' || c == '.') c = ' ';
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c < ' ' || c > 'Z') continue;      // outside the compiled charset
        clean[n++] = c;
    }
    clean[n] = 0;

    // Step the hero DOWN to fit rather than truncating it: a route called
    // "MISSION DOL.." tells the rider less than the same name a size smaller.
    // Descending, like every other step-down ladder here: Impact_B (81) is
    // TALLER than Impact_M (69), so listing M first stepped a name that would
    // have fitted the bigger face straight down to it.
    static const EpdFont* const heroSteps[] = {&Impact_B, &Impact_M, &Impact_T};
    const EpdFont* hf = &Impact_T;
    for (const EpdFont* f : heroSteps)
        if (ui::textWidth(f, clean) <= W - 2 * ui::MARGIN) { hf = f; break; }
    char fit[48];
    fitText(hf, clean, W - 2 * ui::MARGIN, fit, sizeof(fit));
    const int heroH = epdc_digit_height(hf);
    ui::text(hf, x, y + heroH, fit, fb, EPD_DRAW_ALIGN_LEFT, ui::INK);
    y += heroH + 12;

    if (detail && detail[0]) {
        char line[72];
        fitText(&Arial_B, detail, W - 2 * ui::MARGIN, line, sizeof(line));
        ui::text(&Arial_B, x, y + Arial_B.ascender, line, fb,
                 EPD_DRAW_ALIGN_LEFT, ui::INK);
    }
}

void ui_render_power_sheet(bool recording, uint8_t* fb) {
    sheetShell(fb);
    sheetHead("POWER", "SHUT DOWN?",
              recording ? "A ride is recording — it will be saved first."
                        : "The device sleeps until you press the button.",
              fb);
    sheetButton(kPowerShutdown, "SHUT DOWN", true, fb);
    sheetButton(kPowerCancel, "CANCEL", false, fb);
}

void ui_render_shutdown_screen(uint8_t* fb) {
    const int W = epd_rotated_display_width();
    // A solid band so the text stays legible over the map backdrop behind it.
    const int bandY = 402, bandH = 116;
    epd_fill_rect({0, bandY, W, bandH}, 0x00, fb);
    epd_fill_rect({0, bandY - 3, W, 3}, 0xFF, fb);
    epd_fill_rect({0, bandY + bandH, W, 3}, 0xFF, fb);
    ui::label(W / 2, bandY + 50, "POWERED OFF", fb, 0xFF, &Arial_B);
    ui::text(&Arial_L, W / 2, bandY + 90, "press the BOOT button to wake", fb,
             EPD_DRAW_ALIGN_CENTER, 0xFF);   // white on the black band
}

// The app icon, drawn with primitives so the device and the phone show the same
// mark without carrying a bitmap. Proportions measured off
// companion-ios/.../AppIcon-1024.png: within a 290x358 shape the border is 30,
// the compass ring sits at centre (144.5, 208.5) with outer radius 86.5 and
// thickness 21, the stem is 24 wide in two segments with a notch between, and
// the heading arrow spans 75x92. All scaled from that reference here.
//
// A 264 px bitmap of the same thing cost 35 KB of flash and resampled badly on a
// 4-grey panel; this costs nothing and stays crisp at any size.
static void drawAppIcon(int cx, int topY, int height, uint8_t* fb) {
    const float k = (float)height / 358.0f;          // reference shape is 358 tall
    auto S = [&](float v) { return (int)(v * k + 0.5f); };
    const int W = S(290);
    const int x0 = cx - W / 2, y0 = topY;

    // Rounded rectangle, filled — no primitive for it, so a cross of two rects
    // plus four corner circles.
    auto roundRect = [&](int x, int y, int w, int h, int r, uint8_t c) {
        epd_fill_rect({x + r, y, w - 2 * r, h}, c, fb);
        epd_fill_rect({x, y + r, w, h - 2 * r}, c, fb);
        epd_fill_circle(x + r, y + r, r, c, fb);
        epd_fill_circle(x + w - 1 - r, y + r, r, c, fb);
        epd_fill_circle(x + r, y + h - 1 - r, r, c, fb);
        epd_fill_circle(x + w - 1 - r, y + h - 1 - r, r, c, fb);
    };

    const int border = S(30), radius = S(46);
    roundRect(x0, y0, W, S(358), radius, 0x00);                     // shell
    roundRect(x0 + border, y0 + border, W - 2 * border, S(358) - 2 * border,
              radius - border > 2 ? radius - border : 2, 0xFF);     // inner face

    // Stem: two segments with a notch, joining the shell to the compass.
    const int stemW = S(24), stemX = cx - stemW / 2;
    epd_fill_rect({stemX, y0 + S(30), stemW, S(37)}, 0x00, fb);
    epd_fill_rect({stemX, y0 + S(94), stemW, S(40)}, 0x00, fb);

    // Compass ring.
    const int ringX = x0 + S(144.5f), ringY = y0 + S(208.5f);
    epd_fill_circle(ringX, ringY, S(86.5f), 0x00, fb);
    epd_fill_circle(ringX, ringY, S(65.5f), 0xFF, fb);

    // Heading arrow. The accent is orange in the app; on this panel only
    // 0x00..0x33 render at all, so it takes the mid tone — black would collapse
    // it into the ring and lose the two-tone reading.
    const int apexY = y0 + S(163), baseY = y0 + S(255), notchY = y0 + S(230);
    const int lx = x0 + S(107), rx = x0 + S(182);
    epd_fill_triangle(cx, apexY, lx, baseY, cx, notchY, 0x33, fb);
    epd_fill_triangle(cx, apexY, rx, baseY, cx, notchY, 0x33, fb);
}

// Boot progress screen. Drawn from setup() as each subsystem comes up, so the
// glass shows what the firmware is doing instead of holding the previous
// session's image for the several seconds setup() takes. That wait is worst
// exactly when something is wrong — a failing SD mount alone burns ~1.5 s in
// sdWait() timeouts — which is precisely when a blank-looking device is most
// alarming.
//
// Composition: the mark and wordmark sit on the upper third, the step list runs
// from the optical centre, and the version anchors the foot. Only 0x00 and the
// 0x22/0x33 greys are used — this panel renders 0x44 and lighter as plain white.
static void drawAppIcon(int cx, int topY, int height, uint8_t* fb);

void ui_render_boot_screen(const char* version, const char* const* lines,
                           const int8_t* state, const char (*detail)[28],
                           const uint32_t* ms, int count, uint8_t* fb) {
    const int W = epd_rotated_display_width();
    const int H = epd_rotated_display_height();
    epd_fill_rect({0, 0, W, H}, ui::PAPER, fb);

    // Kernel-style boot log. The rider cannot fix a boot problem, but they can
    // SEE one — which step is slow, which subsystem failed, how long the card
    // took. That is worth more on a device with no console than a progress bar
    // is, and it is why the timings are on the glass rather than only in the
    // diag file.
    // Mark and wordmark first — this is still the device's front door, and the
    // log is what it is doing, not what it is. Both sized to leave the log the
    // lower two thirds.
    const int cx = W / 2;
    drawAppIcon(cx, ui::MARGIN + ui::STEP, 168, fb);
    ui::label(cx, 268, "OPEN TRAIL PAPER", fb, ui::INK, &Impact_T);
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "firmware %s · esp32-s3 · 540x960 epd", version);
    ui::text(&Arial_L, cx, 296, hdr, fb, EPD_DRAW_ALIGN_CENTER, ui::INK);

    int y = 320;
    epd_fill_rect({0, y, W, ui::RULE}, ui::INK, fb);
    y += ui::STEP + Arial_L.ascender + 6;

    const int lineH = Arial_L.ascender + 14;
    const int statusX = W - ui::MARGIN;
    for (int i = 0; i < count; ++i) {
        char stamp[16];
        snprintf(stamp, sizeof(stamp), "[%3lu.%02lu]",
                 (unsigned long)(ms[i] / 1000), (unsigned long)((ms[i] % 1000) / 10));
        ui::text(&Arial_L, ui::MARGIN, y, stamp, fb, EPD_DRAW_ALIGN_LEFT, ui::INK);

        char body[64];
        if (detail && detail[i] && detail[i][0])
            snprintf(body, sizeof(body), "%s  %s", lines[i], detail[i]);
        else
            snprintf(body, sizeof(body), "%s", lines[i]);

        // Status bracket, right-aligned: the column the eye scans.
        const char* st = state[i] == BOOT_OK ? "[ OK ]"
                       : state[i] == BOOT_FAIL ? "[FAIL]" : "[ .. ]";

        // Clamp the body to what is left before that column. A long detail used
        // to run straight under the bracket — "42 rides[ OK ]" with no gap —
        // and the status is the part that has to stay readable, because it is
        // the whole reason someone is looking at this screen.
        const int bodyX = ui::MARGIN + 82;
        const int bodyMax = statusX - ui::textWidth(&Arial_B, st) - ui::STEP - bodyX;
        char fitted[64];
        fitText(&Arial_B, body, bodyMax, fitted, sizeof(fitted));
        ui::text(&Arial_B, bodyX, y, fitted, fb, EPD_DRAW_ALIGN_LEFT, ui::INK);
        ui::text(&Arial_B, statusX, y, st, fb, EPD_DRAW_ALIGN_RIGHT, ui::INK);
        y += lineH;
    }

    // Cursor block on the line still running, so a long step reads as busy
    // rather than as a frozen list.
    if (count > 0 && state[count - 1] == BOOT_PENDING)
        epd_fill_rect({ui::MARGIN, y - Arial_L.ascender, 10, Arial_L.ascender},
                      ui::INK, fb);
}

void ui_render_nav_prompt(const char* routeName, int turns, uint8_t* fb) {
    char sub[64];
    snprintf(sub, sizeof(sub), "%d turn%s · tap START to follow it", turns,
             turns == 1 ? "" : "s");
    sheetShell(fb, /*scrim=*/false);   // the route preview must stay readable
    // The route name is the hero line here, at impact_96 like the summary's
    // distance — it was previously set at body size and unreadable at a glance.
    sheetHead("NAVIGATE", routeName, sub, fb);
    sheetButton(kPowerShutdown, "START", true, fb);
    sheetButton(kPowerCancel, "LATER", false, fb);
}

void ui_render_nav_banner(const char* instruction, float distanceM,
                          bool useMiles, uint8_t* fb) {
    const int W = epd_rotated_display_width();
    const int top = ui::STATUS_H;
    const int h = 138;
    epd_fill_rect({0, top, W, h}, 0x00, fb);   // inverted band

    // Direction arrow inferred from the instruction text.
    bool left = strstr(instruction, "left") || strstr(instruction, "Left");
    bool right = strstr(instruction, "right") || strstr(instruction, "Right");
    int ax = 60, ay = top + 60;
    if (left || right) {
        // Turn arrow: a stem that sweeps through a quarter-circle into the head,
        // so it reads like a road bending rather than a right-angled elbow.
        //
        // The curve is stamped as a run of filled circles along the arc. That
        // gives a constant stroke weight and round joins for free, and avoids
        // needing a polygon rasteriser — epdiy only offers rects, triangles and
        // circles.
        //
        // NOTE EpdRect.width is UNSIGNED. An earlier version drew the arm as
        // {ax, y, dir * 30, 8} with dir = -1 for a left turn; the width wrapped
        // to a huge value, so the arm never landed where it should and the head
        // appeared detached. Nothing here passes a signed width.
        const int dir = left ? -1 : 1;
        const int strokeW = 11;             // stroke weight
        const int r = strokeW / 2;
        const int R = 26;                   // radius of the turn
        const int arcTopY = ay + 6;         // where the stem meets the curve
        const int stemBottom = ay + 34;
        const int headLen = 18, headHalf = 15;

        // Straight stem, from the bottom up to where the curve begins.
        epd_fill_rect({ax - r, arcTopY, strokeW, stemBottom - arcTopY}, 0xFF, fb);

        // Quarter arc. Centre sits beside the stem on the side we turn toward,
        // so theta = 0 lands exactly on the stem top and theta = 90 ends
        // travelling horizontally, ready for the head.
        const int cx = ax + dir * R;
        for (int i = 0; i <= 24; ++i) {
            const float t = (float)i / 24.0f * (float)M_PI / 2.0f;
            const int px = cx - (int)lroundf(dir * R * cosf(t));
            const int py = arcTopY - (int)lroundf(R * sinf(t));
            epd_fill_circle(px, py, r, 0xFF, fb);
        }

        // Head at the end of the arc, pointing the way we turn. Its base sits ON
        // the arc's last stamp, so body and head are always joined.
        const int endX = cx, endY = arcTopY - R;
        epd_fill_triangle(endX + dir * headLen, endY,
                          endX, endY - headHalf,
                          endX, endY + headHalf, 0xFF, fb);
    } else {
        // Straight ahead: head on top of a stem, overlapping so they join.
        const int stemW = 10, headHalf = 17, headH = 20;
        epd_fill_rect({ax - stemW / 2, ay - 6, stemW, 34}, 0xFF, fb);
        epd_fill_triangle(ax, ay - 6 - headH,
                          ax - headHalf, ay - 4,
                          ax + headHalf, ay - 4, 0xFF, fb);
    }

    // Arrow and distance form the left cluster; the INSTRUCTION sits to their
    // right at the largest size that fits. It used to run full-width along the
    // bottom of the band in the 14 pt label face — the one thing the rider needs
    // to read at speed, set smaller than everything around it.
    char draw[16], d[16];
    units::navDist(draw, sizeof(draw), distanceM, useMiles);
    upperCopy(d, sizeof(d), draw);
    const int distX = 104;
    ui::text(&Impact_T, distX, top + 78, d, fb, EPD_DRAW_ALIGN_LEFT, 0xFF);

    const int textX = distX + ui::textWidth(&Impact_T, d) + 28;
    const int textW = W - textX - ui::MARGIN;

    // Two lines if one will not do: a turn instruction is a phrase, and
    // shrinking it to a single line costs more legibility than wrapping does.
    char instrU[80];
    upperCopy(instrU, sizeof(instrU), instruction);

    // Prefer the LARGEST face that fits in at most two lines, not the largest
    // that fits on one. Choosing by single-line fit dropped a turn instruction
    // to the 14 pt label face while a 40 pt one would have fitted across two —
    // and this is the single most important string on the screen.
    auto wrapAt = [&](const EpdFont* f, const char* t, char* a, size_t an,
                      char* b, size_t bn) -> bool {
        const size_t n = strlen(t);
        if (ui::textWidth(f, t) <= textW) { snprintf(a, an, "%s", t); b[0] = 0; return true; }
        size_t cut = 0;
        for (size_t i = 0; i < n; ++i) {
            char probe[96];
            if (i + 1 >= sizeof(probe)) break;
            memcpy(probe, t, i + 1);
            probe[i + 1] = 0;
            if (ui::textWidth(f, probe) > textW) break;
            if (t[i] == ' ') cut = i;
        }
        if (!cut) return false;                       // no break point that fits
        snprintf(a, an, "%.*s", (int)cut, t);
        snprintf(b, bn, "%s", t + cut + 1);
        return ui::textWidth(f, b) <= textW;          // tail must fit too
    };

    static const EpdFont* const steps[] = {&Impact_T, &Arial_B, &Arial_L};
    const EpdFont* f = &Arial_L;
    char l1[96] = "", l2[96] = "";
    for (const EpdFont* c : steps) {
        const char* t = (c == &Impact_T) ? instrU : instruction;
        char a[96], b[96];
        if (wrapAt(c, t, a, sizeof(a), b, sizeof(b))) {
            f = c;
            snprintf(l1, sizeof(l1), "%s", a);
            snprintf(l2, sizeof(l2), "%s", b);
            break;
        }
    }
    if (!l1[0]) fitText(f, instruction, textW, l1, sizeof(l1));

    if (l2[0]) {
        ui::text(f, textX, top + 58, l1, fb, EPD_DRAW_ALIGN_LEFT, 0xFF);
        ui::text(f, textX, top + 108, l2, fb, EPD_DRAW_ALIGN_LEFT, 0xFF);
    } else {
        ui::text(f, textX, top + 82, l1, fb, EPD_DRAW_ALIGN_LEFT, 0xFF);
    }
}

void ui_render_pause_banner(uint32_t pausedForS, uint8_t* fb) {
    // Same inverted band, in the same place, as the turn banner — the rider
    // already knows that band means "the device is telling you something".
    // Left cluster is a pause glyph where the arrow would be, then how long
    // the ride has been paused where the turn distance would be; the counter
    // ticking up each second is what says "paused and watching", not hung.
    const int W = epd_rotated_display_width();
    const int top = ui::STATUS_H;
    epd_fill_rect({0, top, W, 138}, 0x00, fb);

    // Pause glyph: two bars, sized to read at a glance from the saddle.
    const int ax = 60, ay = top + 66;
    epd_fill_rect({ax - 19, ay - 24, 14, 48}, 0xFF, fb);
    epd_fill_rect({ax + 5, ay - 24, 14, 48}, 0xFF, fb);

    char d[16];
    if (pausedForS >= 3600) {
        snprintf(d, sizeof(d), "%lu:%02lu:%02lu", (unsigned long)(pausedForS / 3600),
                 (unsigned long)((pausedForS / 60) % 60),
                 (unsigned long)(pausedForS % 60));
    } else {
        snprintf(d, sizeof(d), "%lu:%02lu", (unsigned long)(pausedForS / 60),
                 (unsigned long)(pausedForS % 60));
    }
    const int distX = 104;
    ui::text(&Impact_T, distX, top + 78, d, fb, EPD_DRAW_ALIGN_LEFT, 0xFF);

    const int textX = distX + ui::textWidth(&Impact_T, d) + 28;
    const int textW = W - textX - ui::MARGIN;
    const char* label = "AUTO-PAUSED";
    const EpdFont* f = ui::textWidth(&Impact_T, label) <= textW ? &Impact_T
                                                                : &Arial_B;
    ui::text(f, textX, top + 78, label, fb, EPD_DRAW_ALIGN_LEFT, 0xFF);
}
