#include "ui_render.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "config.h"
#include "ride_state.h"
#include "fonts/arialbold_14.h"
#include "fonts/arialbold_20.h"
#include "fonts/impact_40.h"
#include "fonts/impact_128.h"

// Summary footer touch targets (design 1g: two 100+ px tall actions)
const EpdRect kResumeButton = {0, 830, 180, 130};
const EpdRect kSaveButton = {180, 830, 180, 130};
const EpdRect kDiscardButton = {360, 830, 180, 130};

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
void label(int cx, int y, const char* str, uint8_t* fb, uint8_t color,
           const EpdFont* font) {
    if (!font) font = &ArialBold_14;
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
    int uw = unit && unit[0] ? textWidth(&ArialBold_14, unit) + 10 : 0;
    int startX = x0 + ((x1 - x0) - (vw + uw)) / 2;
    if (startX < x0) startX = x0;   // never spill past the left bound / screen edge
    text(valueFont, startX, baselineY, value, fb, EPD_DRAW_ALIGN_LEFT, color);
    if (uw) {
        text(&ArialBold_14, startX + vw + 10, baselineY, unit, fb,
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

void statusBar(const RideState& s, uint8_t* fb) {
    const int W = epd_rotated_display_width();

    // Clock from GPS time (local), 24h or 12h per the setting.
    char clock[10] = "--:--";
    if (s.timeValid) {
        time_t local = s.utc + (time_t)s.tzMin * 60;
        struct tm tmv;
        gmtime_r(&local, &tmv);
        if (s.clock24h) {
            snprintf(clock, sizeof(clock), "%d:%02d", tmv.tm_hour, tmv.tm_min);
        } else {
            int h = tmv.tm_hour % 12;
            if (h == 0) h = 12;
            snprintf(clock, sizeof(clock), "%d:%02d%c", h, tmv.tm_min,
                     tmv.tm_hour < 12 ? 'a' : 'p');
        }
    }
    text(&ArialBold_14, 16, 40, clock, fb);

    // Companion-app connection: a small phone glyph just after the clock (on the
    // left, out of the crowded battery cluster). Absent = not connected.
    if (s.phoneConnected) {
        const int pw = 15, ph = 26, px = 96, py = 30 - ph / 2;
        epd_fill_rect({px, py, pw, ph}, 0x00, fb);            // phone body
        epd_fill_rect({px + 3, py + 4, pw - 6, ph - 10}, 0xFF, fb);  // screen
        epd_fill_circle(px + pw / 2, py + ph - 4, 1, 0xFF, fb);      // home dot
    }

    // GPS signal dots + sensor labels, left-anchored after the clock/phone. The
    // "GPS" text label is dropped — the dots read as signal strength and the
    // saved width keeps "· PWR" clear of the battery %.
    int dotsW = 4 * 16;
    int x = 128;
    int bars = s.gpsFix ? (s.satellites >= 9 ? 4 : s.satellites >= 6 ? 3
                           : s.satellites >= 4 ? 2 : 1)
                        : 0;
    for (int i = 0; i < 4; ++i) {
        if (i < bars) epd_fill_circle(x + i * 16, 32, 5, 0x00, fb);
        else epd_draw_circle(x + i * 16, 32, 5, 0x00, fb);
    }
    x += dotsW;
    // The "· HR" / "· PWR" labels only appear when connected, so they need no
    // extra checkmark — keeping them text-only frees room for the battery %.
    if (s.hrConnected) {
        text(&ArialBold_14, x, 40, " · HR", fb);
        x += textWidth(&ArialBold_14, " · HR");
    }
    if (s.powerConnected) {
        text(&ArialBold_14, x, 40, " · PWR", fb);
        x += textWidth(&ArialBold_14, " · PWR");
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
    text(&ArialBold_14, pctRight, 40, pct, fb, EPD_DRAW_ALIGN_RIGHT);

    // Lightning bolt left of the % when charging.
    if (s.charging) {
        int pctLeft = pctRight - textWidth(&ArialBold_14, pct);
        drawBolt(pctLeft - 10, 30, 0x00, fb);
    }

    epd_fill_rect({0, STATUS_H - 3, W, 3}, 0x00, fb);
}

}  // namespace ui

namespace {

void formatHms(char* out, size_t len, uint32_t secs) {
    snprintf(out, len, "%lu:%02lu:%02lu", (unsigned long)(secs / 3600),
             (unsigned long)((secs / 60) % 60), (unsigned long)(secs % 60));
}

// One bordered grid cell: tracked label on top, big value below.
void cell(int x0, int y0, int x1, int y1, const char* labelStr,
          const char* value, const char* unit, uint8_t* fb) {
    int cx = (x0 + x1) / 2;
    ui::label(cx, y0 + 36, labelStr, fb);
    // Big value centered in the space between the label and the bottom-anchored
    // unit caption; the unit sits at the cell bottom. Always reserve the unit
    // slot (even when there's no unit) so the value baseline lines up across
    // cells — e.g. RIDE TIME aligns with DISTANCE beside it. The Impact_40
    // baseline sits ~half a cap-height below the target center.
    int unitTop = y1 - 28;
    int vcy = (y0 + 46 + unitTop) / 2;
    ui::text(&Impact_40, cx, vcy + 14, value, fb, EPD_DRAW_ALIGN_CENTER, 0x00);
    if (unit && unit[0]) {
        ui::text(&ArialBold_14, cx, y1 - 12, unit, fb, EPD_DRAW_ALIGN_CENTER, 0x00);
    }
}

}  // namespace

void ui_render_dashboard(const RideState& s, bool navActive, uint8_t* fb) {
    const int W = epd_rotated_display_width();
    const int H = epd_rotated_display_height();
    char buf[32];

    ui::statusBar(s, fb);

    // --- Hero cell: 3 s power, or speed if no power meter --------------
    const int heroBottom = 448;
    bool powerHero = s.powerConnected;
    char speedHdr[16];
    snprintf(speedHdr, sizeof(speedHdr), "SPEED · %s", units::speedLabel(s.useMiles));

    if (navActive) {
        // The turn banner owns the top 138 px (STATUS_H .. STATUS_H+138), so
        // draw a compact hero beneath it instead of the full-size one.
        const int top = ui::STATUS_H + 138;   // 202
        ui::label(W / 2, top + 34, powerHero ? "POWER · 3S" : speedHdr, fb);
        if (powerHero) {
            if (s.power3sW != 0xFFFF) snprintf(buf, sizeof(buf), "%u", s.power3sW);
            else snprintf(buf, sizeof(buf), "--");
            ui::valueWithUnit(&Impact_40, 10, W - 10, top + 150, buf, "W", fb);
        } else {
            snprintf(buf, sizeof(buf), "%.1f", units::speed(s.speedKmh, s.useMiles));
            ui::valueWithUnit(&Impact_40, 10, W - 10, top + 150, buf, "", fb);
        }
        epd_fill_rect({0, heroBottom, W, 3}, 0x00, fb);
    } else {
    ui::label(W / 2, ui::STATUS_H + 44, powerHero ? "POWER · 3S" : speedHdr, fb);

    if (powerHero) {
        if (s.power3sW != 0xFFFF) snprintf(buf, sizeof(buf), "%u", s.power3sW);
        else snprintf(buf, sizeof(buf), "--");
        ui::valueWithUnit(&Impact_128, 10, W - 10, 344, buf, "W", fb);

        // Power zone bar: 7 segments under the hero number
        int ftp = s.ftpW;
        int zone = 0;
        if (s.power3sW != 0xFFFF && ftp > 0) {
            float pct = 100.0f * s.power3sW / ftp;
            zone = pct < 55 ? 1 : pct < 75 ? 2 : pct < 90 ? 3 : pct < 105 ? 4
                   : pct < 120 ? 5 : pct < 150 ? 6 : 7;
        }
        const int segW = (W - 32 - 6 * 8) / 7;
        for (int i = 0; i < 7; ++i) {
            EpdRect seg = {16 + i * (segW + 8), 396, segW, 18};
            if (i < zone) epd_fill_rect(seg, 0x00, fb);
            else {
                epd_draw_rect(seg, 0x00, fb);
            }
        }
    } else {
        // The huge Impact-128 hero only fits ~3 glyphs, so keep the decimal
        // only for single-digit speeds; double digits show as a whole number.
        double spd = units::speed(s.speedKmh, s.useMiles);
        snprintf(buf, sizeof(buf), spd < 10.0 ? "%.1f" : "%.0f", spd);
        ui::valueWithUnit(&Impact_128, 10, W - 10, 360, buf, "", fb);
    }
    }

    epd_fill_rect({0, heroBottom, W, 3}, 0x00, fb);

    // --- 2 x 2 grid -----------------------------------------------------
    // (Elevation/grade removed — GPS altitude with no barometer is unreliable.)
    const int gridTop = heroBottom + 3;
    const int rows[3] = {gridTop, (gridTop + H) / 2, H};
    const int midX = W / 2;

    // Row separator + center divider
    epd_fill_rect({0, rows[1], W, 3}, 0x00, fb);
    epd_fill_rect({midX - 1, rows[0], 3, H - rows[0]}, 0x00, fb);

    if (s.heartRateBpm != 0xFF) snprintf(buf, sizeof(buf), "%u", s.heartRateBpm);
    else snprintf(buf, sizeof(buf), "--");
    cell(0, rows[0], midX, rows[1], "HEART RATE", buf, "BPM", fb);

    if (powerHero) snprintf(buf, sizeof(buf), "%.1f",
                            units::speed(s.speedKmh, s.useMiles));
    else if (s.cadenceRpm != 0xFF) snprintf(buf, sizeof(buf), "%u", s.cadenceRpm);
    else snprintf(buf, sizeof(buf), "--");
    cell(midX, rows[0], W, rows[1], powerHero ? "SPEED" : "CADENCE", buf,
         powerHero ? units::speedLabel(s.useMiles) : "RPM", fb);

    formatHms(buf, sizeof(buf), s.elapsedS);
    cell(0, rows[1], midX, rows[2], "RIDE TIME", buf, "", fb);

    snprintf(buf, sizeof(buf), "%.1f", units::distM(s.distanceM, s.useMiles));
    cell(midX, rows[1], W, rows[2], "DISTANCE", buf, units::distLabel(s.useMiles), fb);
}

void ui_render_summary(const RideSummary& r, uint8_t* fb) {
    const int W = epd_rotated_display_width();
    char buf[40];

    // Inverted header band
    epd_fill_rect({0, 0, W, 150}, 0x00, fb);
    ui::label(W / 2, 66, "RIDE COMPLETE", fb, 0xFF, &ArialBold_20);
    struct tm t0, t1;
    time_t a = r.startUtc + (time_t)r.tzMin * 60;
    time_t b = r.endUtc + (time_t)r.tzMin * 60;
    gmtime_r(&a, &t0);
    gmtime_r(&b, &t1);
    static const char* MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    snprintf(buf, sizeof(buf), "%d %s · %d:%02d - %d:%02d", t0.tm_mday,
             MON[t0.tm_mon], t0.tm_hour, t0.tm_min, t1.tm_hour, t1.tm_min);
    ui::text(&ArialBold_20, W / 2, 116, buf, fb, EPD_DRAW_ALIGN_CENTER, 0xFF);

    // Distance hero
    ui::label(W / 2, 180, "DISTANCE", fb);
    snprintf(buf, sizeof(buf), "%.1f", units::distM(r.distanceM, r.useMiles));
    ui::valueWithUnit(&Impact_128, 10, W - 10, 394, buf, units::distLabel(r.useMiles), fb);
    epd_fill_rect({0, 408, W, 3}, 0x00, fb);

    // 3 x 2 stat grid
    const int rows[4] = {411, 551, 691, 830};
    const int midX = W / 2;
    epd_fill_rect({0, rows[1], W, 3}, 0x00, fb);
    epd_fill_rect({0, rows[2], W, 3}, 0x00, fb);
    epd_fill_rect({midX - 1, rows[0], 3, rows[3] - rows[0]}, 0x00, fb);

    formatHms(buf, sizeof(buf), r.movingS);
    cell(0, rows[0], midX, rows[1], "MOVING TIME", buf, "", fb);
    snprintf(buf, sizeof(buf), "%.1f", units::speed(r.avgSpeedKmh, r.useMiles));
    cell(midX, rows[0], W, rows[1], "AVG SPEED", buf, units::speedLabel(r.useMiles), fb);

    if (r.avgPowerW) snprintf(buf, sizeof(buf), "%u", r.avgPowerW);
    else snprintf(buf, sizeof(buf), "--");
    cell(0, rows[1], midX, rows[2], "AVG POWER", buf, "W", fb);
    if (r.normPowerW) snprintf(buf, sizeof(buf), "%u", r.normPowerW);
    else snprintf(buf, sizeof(buf), "--");
    cell(midX, rows[1], W, rows[2], "NORM. POWER", buf, "W", fb);

    if (r.avgHrBpm) snprintf(buf, sizeof(buf), "%u", r.avgHrBpm);
    else snprintf(buf, sizeof(buf), "--");
    cell(0, rows[2], midX, rows[3], "AVG HR", buf, "BPM", fb);
    // Ascent from the map DEM (accurate, unlike GPS altitude).
    snprintf(buf, sizeof(buf), "%.0f", units::elev(r.climbedM, r.useMiles));
    cell(midX, rows[2], W, rows[3], "ASCENT", buf, units::elevLabel(r.useMiles), fb);

    // Footer actions: RESUME bordered, SAVE inverted (primary), DISCARD bordered.
    // Top border across the whole footer row, plus dividers between the three.
    epd_fill_rect({0, kResumeButton.y, W, 3}, 0x00, fb);
    epd_fill_rect({kSaveButton.x - 1, kResumeButton.y, 3, kResumeButton.height}, 0x00, fb);
    epd_fill_rect({kDiscardButton.x - 1, kResumeButton.y, 3, kResumeButton.height}, 0x00, fb);

    // ArialBold_14 (not _20): "RESUME"/"DISCARD" in _20 are wider than the
    // 180px columns and overlap into neighbouring buttons.
    ui::label(kResumeButton.x + kResumeButton.width / 2,
              kResumeButton.y + kResumeButton.height / 2 + 6, "RESUME", fb,
              0x00, &ArialBold_14);

    epd_fill_rect({kSaveButton.x, kSaveButton.y, kSaveButton.width,
                   kSaveButton.height}, 0x00, fb);
    ui::label(kSaveButton.x + kSaveButton.width / 2,
              kSaveButton.y + kSaveButton.height / 2 + 6, "SAVE", fb, 0xFF,
              &ArialBold_14);

    ui::label(kDiscardButton.x + kDiscardButton.width / 2,
              kDiscardButton.y + kDiscardButton.height / 2 + 6, "DISCARD", fb,
              0x00, &ArialBold_14);
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
        return ui::textWidth(&ArialBold_20, s) <= innerW ? &ArialBold_20 : &ArialBold_14;
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

    ui::text(&ArialBold_14, W / 2, by + 270, "Keep the app open and the", fb,
             EPD_DRAW_ALIGN_CENTER, 0x00);
    ui::text(&ArialBold_14, W / 2, by + 300, "device nearby until it finishes.", fb,
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

void ui_render_menu(const MenuInfo& m, uint8_t* fb) {
    const int W = epd_rotated_display_width();
    const int H = epd_rotated_display_height();
    char sub[64];

    // Header: MENU left (inset to match the rows), battery percent right
    ui::text(&ArialBold_20, 44, 60, "MENU", fb, EPD_DRAW_ALIGN_LEFT, 0x00);
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", m.batteryPercent);
    ui::text(&ArialBold_14, W - 16, 54, pct, fb, EPD_DRAW_ALIGN_RIGHT);
    epd_fill_rect({0, kMenuRowTop - 3, W, 3}, 0x00, fb);

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

    for (int i = 0; i < kMenuRowCount; ++i) {
        int y = kMenuRowTop + i * kMenuRowH;
        uint8_t fg = 0x00;
        if (rows[i].inverted) {
            epd_fill_rect({0, y, W, kMenuRowH}, 0x00, fb);
            fg = 0xFF;
        }
        // Left inset for breathing room; pure-contrast subtitle (gray is
        // unreadable on the e-paper panel); truncate so text clears the arrow.
        char mt[48], ms[64];
        fitText(&ArialBold_20, rows[i].title, W - 44 - 44, mt, sizeof(mt));
        fitText(&ArialBold_14, rows[i].subtitle, W - 44 - 44, ms, sizeof(ms));
        ui::text(&ArialBold_20, 44, y + 66, mt, fb, EPD_DRAW_ALIGN_LEFT, fg);
        ui::text(&ArialBold_14, 44, y + 108, ms, fb,
                 EPD_DRAW_ALIGN_LEFT, fg == 0xFF ? 0xFF : 0x00);
        ui::text(&ArialBold_20, W - 28, y + 84, ">", fb,
                 EPD_DRAW_ALIGN_RIGHT, fg);
        if (!rows[i].inverted) {
            epd_fill_rect({0, y + kMenuRowH - 1, W, 1}, 0x00, fb);
        }
    }

    // Footer status line
    if (m.sdOk) {
        snprintf(sub, sizeof(sub), FIRMWARE_VERSION " · %lu MB free · %s",
                 (unsigned long)m.sdFreeMB, m.gpsReady ? "GPS lock" : "no GPS");
    } else {
        snprintf(sub, sizeof(sub), FIRMWARE_VERSION " · no SD card · %s",
                 m.gpsReady ? "GPS lock" : "no GPS");
    }
    ui::text(&ArialBold_14, W / 2, H - 24, sub, fb, EPD_DRAW_ALIGN_CENTER, 0x00);
}

void ui_render_list(const char* title, const ListRow* rows, int count,
                    const char* footer, uint8_t* fb) {
    const int W = epd_rotated_display_width();
    const int H = epd_rotated_display_height();

    ui::text(&ArialBold_20, 28, 60, title, fb);
    ui::text(&ArialBold_14, W - 16, 54, "< BACK", fb, EPD_DRAW_ALIGN_RIGHT,
             0x00);
    epd_fill_rect({0, kMenuRowTop - 3, W, 3}, 0x00, fb);

    for (int i = 0; i < count && i < kMenuRowCount; ++i) {
        int y = kMenuRowTop + i * kMenuRowH;
        uint8_t fg = 0x00;
        if (rows[i].inverted) {
            epd_fill_rect({0, y, W, kMenuRowH}, 0x00, fb);
            fg = 0xFF;
        }
        // Truncate long names/subtitles so they can't run off the row. PURE
        // black/white — grays reproduce poorly on the physical e-paper (fine in
        // previews, faint on the panel); hierarchy comes from the smaller font.
        char t[40], s[64];
        fitText(&ArialBold_20, rows[i].title, W - 28 - 20, t, sizeof(t));
        fitText(&ArialBold_14, rows[i].subtitle, W - 28 - 20, s, sizeof(s));
        ui::text(&ArialBold_20, 28, y + 64, t, fb, EPD_DRAW_ALIGN_LEFT, fg);
        ui::text(&ArialBold_14, 28, y + 106, s, fb,
                 EPD_DRAW_ALIGN_LEFT, fg == 0xFF ? 0xFF : 0x00);
        if (!rows[i].inverted) {
            epd_fill_rect({0, y + kMenuRowH - 1, W, 1}, 0x00, fb);
        }
    }
    if (count == 0) {
        ui::text(&ArialBold_14, W / 2, kMenuRowTop + 80, "nothing found", fb,
                 EPD_DRAW_ALIGN_CENTER, 0x00);
    }

    if (footer && footer[0]) {
        ui::text(&ArialBold_14, W / 2, kBackBar.y - 16, footer, fb,
                 EPD_DRAW_ALIGN_CENTER, 0x00);
    }
    ui_render_back_bar(fb);
}

// Full-width BACK strip along the bottom.
const EpdRect kBackBar = {0, 878, 540, 82};

void ui_render_back_bar(uint8_t* fb) {
    const int W = epd_rotated_display_width();
    epd_fill_rect({0, kBackBar.y, W, 3}, 0x00, fb);
    ui::text(&ArialBold_20, W / 2, kBackBar.y + 54, "< BACK", fb,
             EPD_DRAW_ALIGN_CENTER);
}

// iOS-style pill switch: black filled pill with a white knob on the right
// when on; white outlined pill with a black knob on the left when off.
static void settingsToggle(int x, int y, int w, int h, bool on, uint8_t* fb) {
    const int r = h / 2;
    const int lcx = x + r;          // left cap center
    const int rcx = x + w - r;      // right cap center
    const int cy = y + r;
    const int knobR = r - 6;
    // Solid black pill (rounded rect = middle band + two end caps).
    epd_fill_rect({lcx, y, w - h, h}, 0x00, fb);
    epd_fill_circle(lcx, cy, r, 0x00, fb);
    epd_fill_circle(rcx, cy, r, 0x00, fb);
    if (on) {
        epd_fill_circle(rcx, cy, knobR, 0xFF, fb);   // white knob, right
    } else {
        // Hollow the pill to a thin outline, then a black knob on the left.
        const int t = 3;
        epd_fill_rect({lcx, y + t, w - h, h - 2 * t}, 0xFF, fb);
        epd_fill_circle(lcx, cy, r - t, 0xFF, fb);
        epd_fill_circle(rcx, cy, r - t, 0xFF, fb);
        epd_fill_circle(lcx, cy, knobR, 0x00, fb);
    }
}

void ui_render_settings(const SettingsInfo& si, uint8_t* fb) {
    const int W = epd_rotated_display_width();

    ui::text(&ArialBold_20, 28, 60, "SETTINGS", fb);
    epd_fill_rect({0, kMenuRowTop - 3, W, 3}, 0x00, fb);

    struct Row {
        const char* label;
        char value[16];
        bool toggle;   // switch instead of +/- stepper
        bool on;       // switch position when toggle
    } rows[5];
    snprintf(rows[0].value, sizeof(rows[0].value), "%d W", si.ftpW);
    rows[0].label = "FTP";
    rows[0].toggle = false;
    int tzH = si.tzMin / 60, tzM = abs(si.tzMin % 60);
    if (tzM) snprintf(rows[1].value, sizeof(rows[1].value), "%+d:%02d", tzH, tzM);
    else snprintf(rows[1].value, sizeof(rows[1].value), "UTC%+d", tzH);
    rows[1].label = "TIMEZONE";
    rows[1].toggle = false;
    static const char* BL[] = {"Off", "Low", "Med", "Bright"};
    snprintf(rows[2].value, sizeof(rows[2].value), "%s",
             BL[si.backlight < 0 ? 0 : si.backlight > 3 ? 3 : si.backlight]);
    rows[2].label = "BACKLIGHT";
    rows[2].toggle = true;
    rows[2].on = si.backlight > 0;
    snprintf(rows[3].value, sizeof(rows[3].value), "%s",
             si.useMiles ? "Miles" : "Km");
    rows[3].label = "UNITS";
    rows[3].toggle = true;
    rows[3].on = si.useMiles;
    snprintf(rows[4].value, sizeof(rows[4].value), "%s",
             si.usbDrive ? "On" : "Off");
    rows[4].label = "USB DRIVE";
    rows[4].toggle = true;
    rows[4].on = si.usbDrive;

    for (int i = 0; i < 5; ++i) {
        int y = kMenuRowTop + i * kSettingsRowH;
        ui::text(&ArialBold_14, 28, y + kSettingsRowH / 2 + 5, rows[i].label, fb);

        if (rows[i].toggle) {
            // current value to the left, switch on the right
            int vx = (kSettingsMinusX + kSettingsToggleX) / 2;
            ui::text(&ArialBold_20, vx, y + kSettingsRowH / 2 + 8, rows[i].value,
                     fb, EPD_DRAW_ALIGN_CENTER);
            int ty = y + (kSettingsRowH - kSettingsToggleH) / 2;
            settingsToggle(kSettingsToggleX, ty, kSettingsToggleW,
                           kSettingsToggleH, rows[i].on, fb);
        } else {
            // minus / plus buttons with the value between
            for (int b = 0; b < 2; ++b) {
                int bx = b == 0 ? kSettingsMinusX : kSettingsPlusX;
                EpdRect r = {bx, y + (kSettingsRowH - kSettingsBtn) / 2,
                             kSettingsBtn, kSettingsBtn};
                epd_draw_rect(r, 0x00, fb);
                EpdRect r2 = {r.x + 1, r.y + 1, r.width - 2, r.height - 2};
                epd_draw_rect(r2, 0x00, fb);
                int cx = r.x + r.width / 2, cy = r.y + r.height / 2;
                epd_fill_rect({cx - 12, cy - 2, 24, 5}, 0x00, fb);
                if (b == 1) epd_fill_rect({cx - 2, cy - 12, 5, 24}, 0x00, fb);
            }
            int vx = (kSettingsMinusX + kSettingsBtn + kSettingsPlusX) / 2;
            ui::text(&ArialBold_20, vx, y + kSettingsRowH / 2 + 8, rows[i].value,
                     fb, EPD_DRAW_ALIGN_CENTER);
        }
        epd_fill_rect({0, y + kSettingsRowH - 1, W, 1}, 0x00, fb);
    }

    // Navigation rows below the adjustable settings. (Sensors is on the main
    // menu, not duplicated here.)
    struct NavRow { const char* title; const char* sub; };
    const NavRow navs[2] = {
        {"GPS Debug", "receiver diagnostics"},
        {"Grey Test", "grayscale swatches for tuning"},
    };
    for (int i = 0; i < 2; ++i) {
        int y = kMenuRowTop + (5 + i) * kSettingsRowH;
        ui::text(&ArialBold_20, 28, y + 40, navs[i].title, fb);
        ui::text(&ArialBold_14, 28, y + 70, navs[i].sub, fb,
                 EPD_DRAW_ALIGN_LEFT, 0x00);
        ui::text(&ArialBold_20, W - 28, y + 54, ">", fb, EPD_DRAW_ALIGN_RIGHT);
        epd_fill_rect({0, y + kSettingsRowH - 1, W, 1}, 0x00, fb);
    }

    ui_render_back_bar(fb);
}

// Grayscale test: 16 labelled swatches, white at top to black at bottom, so we
// can see which gray levels the panel actually reproduces. Tap anywhere to
// return. Must be shown with a full-grayscale (GL16/GC16) refresh, not DU.
void ui_render_grey_test(uint8_t* fb) {
    const int W = epd_rotated_display_width();
    const int H = epd_rotated_display_height();
    const int top = 84;

    memset(fb, 0xFF, (size_t)(W / 2) * H);   // clear (4bpp: 2 px/byte)
    ui::text(&ArialBold_20, 28, 60, "GREY TEST", fb);
    epd_fill_rect({0, top - 3, W, 3}, 0x00, fb);

    const int n = 16;
    const int bandH = (H - top) / n;
    for (int i = 0; i < n; ++i) {
        // White (0xFF) at top down to black (0x00). 16 even steps = the panel's
        // native GL16 levels (the value's high nibble is the gray).
        uint8_t v = (uint8_t)((15 - i) * 0x11);
        int y = top + i * bandH;
        epd_fill_rect({0, y, W, bandH}, v, fb);
        char lbl[8];
        snprintf(lbl, sizeof(lbl), "0x%02X", v);
        uint8_t txt = v < 0x88 ? 0xFF : 0x00;   // contrast against the band
        ui::text(&ArialBold_20, 24, y + bandH / 2 + 8, lbl, fb,
                 EPD_DRAW_ALIGN_LEFT, txt);
    }
}

void ui_render_gps_debug(const GpsDebugView& g, uint8_t* fb) {
    const int W = epd_rotated_display_width();

    ui::text(&ArialBold_20, 28, 60, "GPS DEBUG", fb);
    epd_fill_rect({0, kMenuRowTop - 3, W, 3}, 0x00, fb);

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
    DBG_LINE("CHECKSUM OK / FAIL", "%lu / %lu", (unsigned long)g.passedCksum,
             (unsigned long)g.failedCksum);
    DBG_LINE("SENTENCES W/ FIX", "%lu", (unsigned long)g.withFix);
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
    // Spacing is derived so all rows fit between the header and back bar.
    int y0 = kMenuRowTop + 38;
    int step = n > 0 ? (kBackBar.y - 6 - y0) / n : 40;
    for (int i = 0; i < n; ++i) {
        int y = y0 + i * step + step - 20;
        ui::text(&ArialBold_14, 28, y, lines[i].label, fb,
                 EPD_DRAW_ALIGN_LEFT, 0x00);
        ui::text(&ArialBold_20, W - 28, y, lines[i].value, fb,
                 EPD_DRAW_ALIGN_RIGHT, 0x00);
        if (i > 0) epd_fill_rect({0, y0 + i * step, W, 1}, 0x00, fb);
    }

    ui_render_back_bar(fb);
}

// Bottom sheet: ~360 px tall, drawn over the live screen behind it.
const EpdRect kPowerSheet = {0, 600, 540, 360};
const EpdRect kPowerShutdown = {30, 720, 480, 92};
const EpdRect kPowerCancel = {30, 836, 480, 92};

void ui_render_power_sheet(bool recording, uint8_t* fb) {
    const int W = epd_rotated_display_width();

    // Solid white card so the screen behind stays legible above it, with
    // a heavy top rule to separate the sheet from the content.
    epd_fill_rect({kPowerSheet.x, kPowerSheet.y, kPowerSheet.width,
                   kPowerSheet.height}, 0xFF, fb);
    epd_fill_rect({0, kPowerSheet.y, W, 4}, 0x00, fb);

    // Small power glyph on the left of the title
    int gx = 70, gy = kPowerSheet.y + 66;
    for (int r = 26; r <= 30; ++r) epd_draw_circle(gx, gy, r, 0x00, fb);
    epd_fill_rect({gx - 9, gy - 38, 18, 34}, 0xFF, fb);
    epd_fill_rect({gx - 3, gy - 34, 6, 32}, 0x00, fb);

    ui::text(&ArialBold_20, 118, kPowerSheet.y + 58, "Shut down?", fb);
    ui::text(&ArialBold_14, 118, kPowerSheet.y + 98,
             recording ? "your ride will be saved first"
                       : "press BOOT to wake up again",
             fb, EPD_DRAW_ALIGN_LEFT, 0x33);   // 0x33 = darkest grey the panel shows

    // SHUT DOWN — inverted
    epd_fill_rect({kPowerShutdown.x, kPowerShutdown.y, kPowerShutdown.width,
                   kPowerShutdown.height}, 0x00, fb);
    ui::text(&ArialBold_20, kPowerShutdown.x + kPowerShutdown.width / 2,
             kPowerShutdown.y + kPowerShutdown.height / 2 + 10, "SHUT DOWN", fb,
             EPD_DRAW_ALIGN_CENTER, 0xFF);

    // CANCEL — bordered
    for (int i = 0; i < 3; ++i) {
        EpdRect r = {kPowerCancel.x + i, kPowerCancel.y + i,
                     kPowerCancel.width - i * 2, kPowerCancel.height - i * 2};
        epd_draw_rect(r, 0x00, fb);
    }
    ui::text(&ArialBold_20, kPowerCancel.x + kPowerCancel.width / 2,
             kPowerCancel.y + kPowerCancel.height / 2 + 10, "CANCEL", fb,
             EPD_DRAW_ALIGN_CENTER);
}

void ui_render_shutdown_screen(uint8_t* fb) {
    const int W = epd_rotated_display_width();
    // A solid band so the text stays legible over the map backdrop behind it.
    const int bandY = 402, bandH = 116;
    epd_fill_rect({0, bandY, W, bandH}, 0x00, fb);
    epd_fill_rect({0, bandY - 3, W, 3}, 0xFF, fb);
    epd_fill_rect({0, bandY + bandH, W, 3}, 0xFF, fb);
    ui::label(W / 2, bandY + 50, "POWERED OFF", fb, 0xFF, &ArialBold_20);
    ui::text(&ArialBold_14, W / 2, bandY + 90, "press the BOOT button to wake", fb,
             EPD_DRAW_ALIGN_CENTER, 0xFF);   // white on the black band
}

void ui_render_nav_prompt(const char* routeName, int turns, uint8_t* fb) {
    const int W = epd_rotated_display_width();

    epd_fill_rect({kPowerSheet.x, kPowerSheet.y, kPowerSheet.width,
                   kPowerSheet.height}, 0xFF, fb);
    epd_fill_rect({0, kPowerSheet.y, W, 4}, 0x00, fb);

    // Signpost glyph (aligned with the title line)
    int gx = 70, gy = kPowerSheet.y + 34;
    epd_fill_rect({gx - 4, gy - 28, 8, 46}, 0x00, fb);
    epd_fill_triangle(gx + 34, gy - 18, gx - 2, gy - 28, gx - 2, gy - 8, 0x00, fb);

    ui::text(&ArialBold_20, 118, kPowerSheet.y + 42, "Start navigation?", fb);

    // Route name + turn count in an inverted (black) band with breathing room
    // above and below — reads much darker than the old light-grey caption.
    char name[48];
    snprintf(name, sizeof(name), "%s", routeName);
    size_t nl = strlen(name);   // drop a trailing ".gpx" for a cleaner label
    if (nl > 4 && strcmp(name + nl - 4, ".gpx") == 0) name[nl - 4] = 0;
    char sub[64];
    snprintf(sub, sizeof(sub), "%s · %d turns", name, turns);
    char fitted[64];
    fitText(&ArialBold_20, sub, 480 - 28, fitted, sizeof(fitted));
    EpdRect band = {30, kPowerSheet.y + 58, 480, 46};
    epd_fill_rect(band, 0x00, fb);
    ui::text(&ArialBold_20, band.x + band.width / 2, band.y + band.height / 2 + 8,
             fitted, fb, EPD_DRAW_ALIGN_CENTER, 0xFF);

    epd_fill_rect({kPowerShutdown.x, kPowerShutdown.y, kPowerShutdown.width,
                   kPowerShutdown.height}, 0x00, fb);
    ui::text(&ArialBold_20, kPowerShutdown.x + kPowerShutdown.width / 2,
             kPowerShutdown.y + kPowerShutdown.height / 2 + 10, "START", fb,
             EPD_DRAW_ALIGN_CENTER, 0xFF);
    for (int i = 0; i < 3; ++i) {
        EpdRect r = {kPowerCancel.x + i, kPowerCancel.y + i,
                     kPowerCancel.width - i * 2, kPowerCancel.height - i * 2};
        epd_draw_rect(r, 0x00, fb);
    }
    ui::text(&ArialBold_20, kPowerCancel.x + kPowerCancel.width / 2,
             kPowerCancel.y + kPowerCancel.height / 2 + 10, "LATER", fb,
             EPD_DRAW_ALIGN_CENTER);
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
        int dir = left ? -1 : 1;
        // shaft up, head turning left/right
        epd_fill_rect({ax - 4, ay - 4, 8, 34}, 0xFF, fb);
        epd_fill_rect({ax, ay - 4, dir * 30, 8}, 0xFF, fb);
        int hx = ax + dir * 30;
        epd_fill_triangle(hx + dir * 16, ay, hx, ay - 14, hx, ay + 14, 0xFF, fb);
    } else {
        // straight-ahead chevron
        epd_fill_triangle(ax, ay - 18, ax - 16, ay + 8, ax + 16, ay + 8, 0xFF, fb);
        epd_fill_rect({ax - 5, ay + 2, 10, 26}, 0xFF, fb);
    }

    char d[16];
    if (useMiles) {
        float ft = distanceM * 3.28084f;
        if (ft >= 1000) snprintf(d, sizeof(d), "%.1f mi", distanceM * 0.000621371f);
        else snprintf(d, sizeof(d), "%d ft", (int)(ft + 0.5f));
    } else if (distanceM >= 1000) {
        snprintf(d, sizeof(d), "%.1f km", distanceM / 1000);
    } else {
        snprintf(d, sizeof(d), "%d m", (int)(distanceM + 0.5f));
    }
    ui::text(&ArialBold_20, 108, top + 52, d, fb, EPD_DRAW_ALIGN_LEFT, 0xFF);
    char instr[48];
    fitText(&ArialBold_14, instruction, W - 24 - 12, instr, sizeof(instr));
    ui::text(&ArialBold_14, 24, top + 112, instr, fb, EPD_DRAW_ALIGN_LEFT, 0xFF);
}
