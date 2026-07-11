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
const EpdRect kSaveButton = {0, 830, 270, 130};
const EpdRect kDiscardButton = {270, 830, 270, 130};

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

}  // namespace

void statusBar(const RideState& s, uint8_t* fb) {
    const int W = epd_rotated_display_width();

    // Clock from GPS time (local)
    char clock[8] = "--:--";
    if (s.timeValid) {
        time_t local = s.utc + (time_t)s.tzMin * 60;
        struct tm tmv;
        gmtime_r(&local, &tmv);
        snprintf(clock, sizeof(clock), "%d:%02d", tmv.tm_hour, tmv.tm_min);
    }
    text(&ArialBold_14, 16, 40, clock, fb);

    // Center: GPS dots + sensor checks, e.g. "GPS ooo. HR/ PWR/"
    int cx = W / 2;
    char sensors[24];
    snprintf(sensors, sizeof(sensors), "%s%s", s.hrConnected ? " · HR" : "",
             s.powerConnected ? " · PWR" : "");
    int gpsTextW = textWidth(&ArialBold_14, "GPS");
    int dotsW = 4 * 16;
    int hrW = sensors[0] ? textWidth(&ArialBold_14, sensors) : 0;
    int checks = (s.hrConnected ? 1 : 0) + (s.powerConnected ? 1 : 0);
    int total = gpsTextW + 8 + dotsW + hrW + checks * 20;
    int x = cx - total / 2;

    text(&ArialBold_14, x, 40, "GPS", fb);
    x += gpsTextW + 10;
    int bars = s.gpsFix ? (s.satellites >= 9 ? 4 : s.satellites >= 6 ? 3
                           : s.satellites >= 4 ? 2 : 1)
                        : 0;
    for (int i = 0; i < 4; ++i) {
        if (i < bars) epd_fill_circle(x + i * 16, 32, 5, 0x00, fb);
        else epd_draw_circle(x + i * 16, 32, 5, 0x00, fb);
    }
    x += dotsW;
    if (s.hrConnected) {
        text(&ArialBold_14, x, 40, " · HR", fb);
        x += textWidth(&ArialBold_14, " · HR") + 4;
        check(x, 38, fb);
        x += 20;
    }
    if (s.powerConnected) {
        text(&ArialBold_14, x, 40, " · PWR", fb);
        x += textWidth(&ArialBold_14, " · PWR") + 4;
        check(x, 38, fb);
        x += 20;
    }

    batteryIcon(W - 12, 30, s.batteryPercent, s.charging, fb);

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
    ui::label(cx, y0 + 40, labelStr, fb);
    ui::valueWithUnit(&Impact_40, x0 + 8, x1 - 8, y1 - 28, value, unit, fb);
}

}  // namespace

void ui_render_dashboard(const RideState& s, uint8_t* fb) {
    const int W = epd_rotated_display_width();
    const int H = epd_rotated_display_height();
    char buf[32];

    ui::statusBar(s, fb);

    // --- Hero cell: 3 s power, or speed if no power meter --------------
    const int heroBottom = 448;
    bool powerHero = s.powerConnected;
    ui::label(W / 2, ui::STATUS_H + 44, powerHero ? "POWER · 3S" : "SPEED · KM/H",
              fb);

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
        snprintf(buf, sizeof(buf), "%.1f", s.speedKmh);
        ui::valueWithUnit(&Impact_128, 10, W - 10, 360, buf, "", fb);
    }

    epd_fill_rect({0, heroBottom, W, 3}, 0x00, fb);

    // --- 3 x 2 grid -----------------------------------------------------
    const int rows[4] = {heroBottom + 3, 620, 790, H};
    const int midX = W / 2;

    // Row separators + center divider
    epd_fill_rect({0, rows[1], W, 3}, 0x00, fb);
    epd_fill_rect({0, rows[2], W, 3}, 0x00, fb);
    epd_fill_rect({midX - 1, rows[0], 3, H - rows[0]}, 0x00, fb);

    if (s.heartRateBpm != 0xFF) snprintf(buf, sizeof(buf), "%u", s.heartRateBpm);
    else snprintf(buf, sizeof(buf), "--");
    cell(0, rows[0], midX, rows[1], "HEART RATE", buf, "BPM", fb);

    if (powerHero) snprintf(buf, sizeof(buf), "%.1f", s.speedKmh);
    else if (s.cadenceRpm != 0xFF) snprintf(buf, sizeof(buf), "%u", s.cadenceRpm);
    else snprintf(buf, sizeof(buf), "--");
    cell(midX, rows[0], W, rows[1], powerHero ? "SPEED" : "CADENCE", buf,
         powerHero ? "KM/H" : "RPM", fb);

    formatHms(buf, sizeof(buf), s.elapsedS);
    cell(0, rows[1], midX, rows[2], "RIDE TIME", buf, "", fb);

    snprintf(buf, sizeof(buf), "%.1f", s.distanceM / 1000.0);
    cell(midX, rows[1], W, rows[2], "DISTANCE", buf, "KM", fb);

    if (s.gradeValid) snprintf(buf, sizeof(buf), "%.1f", s.gradePercent);
    else snprintf(buf, sizeof(buf), "--");
    cell(0, rows[2], midX, rows[3], "GRADE", buf, "%", fb);

    snprintf(buf, sizeof(buf), "%.0f", s.altitudeM);
    cell(midX, rows[2], W, rows[3], "ELEVATION", buf, "M", fb);
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
    snprintf(buf, sizeof(buf), "%.1f", r.distanceM / 1000.0);
    ui::valueWithUnit(&Impact_128, 10, W - 10, 394, buf, "KM", fb);
    epd_fill_rect({0, 408, W, 3}, 0x00, fb);

    // 3 x 2 stat grid
    const int rows[4] = {411, 551, 691, 830};
    const int midX = W / 2;
    epd_fill_rect({0, rows[1], W, 3}, 0x00, fb);
    epd_fill_rect({0, rows[2], W, 3}, 0x00, fb);
    epd_fill_rect({midX - 1, rows[0], 3, rows[3] - rows[0]}, 0x00, fb);

    formatHms(buf, sizeof(buf), r.movingS);
    cell(0, rows[0], midX, rows[1], "MOVING TIME", buf, "", fb);
    snprintf(buf, sizeof(buf), "%.1f", r.avgSpeedKmh);
    cell(midX, rows[0], W, rows[1], "AVG SPEED", buf, "KM/H", fb);

    if (r.avgPowerW) snprintf(buf, sizeof(buf), "%u", r.avgPowerW);
    else snprintf(buf, sizeof(buf), "--");
    cell(0, rows[1], midX, rows[2], "AVG POWER", buf, "W", fb);
    if (r.normPowerW) snprintf(buf, sizeof(buf), "%u", r.normPowerW);
    else snprintf(buf, sizeof(buf), "--");
    cell(midX, rows[1], W, rows[2], "NORM. POWER", buf, "W", fb);

    if (r.avgHrBpm) snprintf(buf, sizeof(buf), "%u", r.avgHrBpm);
    else snprintf(buf, sizeof(buf), "--");
    cell(0, rows[2], midX, rows[3], "AVG HR", buf, "BPM", fb);
    snprintf(buf, sizeof(buf), "%.0f", r.climbedM);
    cell(midX, rows[2], W, rows[3], "CLIMBED", buf, "M", fb);

    // Footer actions: SAVE RIDE inverted, DISCARD bordered
    epd_fill_rect({kSaveButton.x, kSaveButton.y, kSaveButton.width,
                   kSaveButton.height}, 0x00, fb);
    ui::label(kSaveButton.x + kSaveButton.width / 2,
              kSaveButton.y + kSaveButton.height / 2 + 10, "SAVE RIDE", fb, 0xFF,
              &ArialBold_20);
    epd_fill_rect({kDiscardButton.x, kDiscardButton.y, kDiscardButton.width, 3},
                  0x00, fb);
    ui::label(kDiscardButton.x + kDiscardButton.width / 2,
              kDiscardButton.y + kDiscardButton.height / 2 + 10, "DISCARD", fb,
              0x00, &ArialBold_20);
}

void ui_render_menu(const MenuInfo& m, uint8_t* fb) {
    const int W = epd_rotated_display_width();
    const int H = epd_rotated_display_height();
    char sub[64];

    // Header: MENU left, battery icon + percent right
    ui::label(70, 60, "MENU", fb, 0x00, &ArialBold_20);
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
        snprintf(startSub, sizeof(startSub), "recording · %.1f km",
                 m.rideDistanceM / 1000.0);
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
        ui::text(&ArialBold_20, 28, y + 66, rows[i].title, fb,
                 EPD_DRAW_ALIGN_LEFT, fg);
        ui::text(&ArialBold_14, 28, y + 108, rows[i].subtitle, fb,
                 EPD_DRAW_ALIGN_LEFT, fg == 0xFF ? 0xC0 : 0x60);
        ui::text(&ArialBold_20, W - 28, y + 84, ">", fb,
                 EPD_DRAW_ALIGN_RIGHT, fg);
        if (!rows[i].inverted) {
            epd_fill_rect({0, y + kMenuRowH - 1, W, 1}, 0x80, fb);
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
    ui::text(&ArialBold_14, W / 2, H - 24, sub, fb, EPD_DRAW_ALIGN_CENTER, 0x60);
}

void ui_render_list(const char* title, const ListRow* rows, int count,
                    const char* footer, uint8_t* fb) {
    const int W = epd_rotated_display_width();
    const int H = epd_rotated_display_height();

    ui::text(&ArialBold_20, 28, 60, title, fb);
    ui::text(&ArialBold_14, W - 16, 54, "< BACK", fb, EPD_DRAW_ALIGN_RIGHT,
             0x60);
    epd_fill_rect({0, kMenuRowTop - 3, W, 3}, 0x00, fb);

    for (int i = 0; i < count && i < kMenuRowCount; ++i) {
        int y = kMenuRowTop + i * kMenuRowH;
        uint8_t fg = 0x00;
        if (rows[i].inverted) {
            epd_fill_rect({0, y, W, kMenuRowH}, 0x00, fb);
            fg = 0xFF;
        }
        ui::text(&ArialBold_20, 28, y + 64, rows[i].title, fb,
                 EPD_DRAW_ALIGN_LEFT, fg);
        ui::text(&ArialBold_14, 28, y + 106, rows[i].subtitle, fb,
                 EPD_DRAW_ALIGN_LEFT, fg == 0xFF ? 0xC0 : 0x60);
        if (!rows[i].inverted) {
            epd_fill_rect({0, y + kMenuRowH - 1, W, 1}, 0x80, fb);
        }
    }
    if (count == 0) {
        ui::text(&ArialBold_14, W / 2, kMenuRowTop + 80, "nothing found", fb,
                 EPD_DRAW_ALIGN_CENTER, 0x60);
    }

    if (footer && footer[0]) {
        ui::text(&ArialBold_14, W / 2, kBackBar.y - 16, footer, fb,
                 EPD_DRAW_ALIGN_CENTER, 0x60);
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

void ui_render_settings(const SettingsInfo& si, uint8_t* fb) {
    const int W = epd_rotated_display_width();

    ui::text(&ArialBold_20, 28, 60, "SETTINGS", fb);
    epd_fill_rect({0, kMenuRowTop - 3, W, 3}, 0x00, fb);

    struct Row {
        const char* label;
        char value[16];
    } rows[3];
    snprintf(rows[0].value, sizeof(rows[0].value), "%d W", si.ftpW);
    rows[0].label = "FTP";
    int tzH = si.tzMin / 60, tzM = abs(si.tzMin % 60);
    if (tzM) snprintf(rows[1].value, sizeof(rows[1].value), "%+d:%02d", tzH, tzM);
    else snprintf(rows[1].value, sizeof(rows[1].value), "UTC%+d", tzH);
    rows[1].label = "TIMEZONE";
    static const char* BL[] = {"Off", "Low", "Med", "Bright"};
    snprintf(rows[2].value, sizeof(rows[2].value), "%s",
             BL[si.backlight < 0 ? 0 : si.backlight > 3 ? 3 : si.backlight]);
    rows[2].label = "FRONTLIGHT";

    for (int i = 0; i < 3; ++i) {
        int y = kMenuRowTop + i * kMenuRowH;
        ui::text(&ArialBold_14, 28, y + 84, rows[i].label, fb);

        // minus / plus buttons with the value between
        for (int b = 0; b < 2; ++b) {
            int bx = b == 0 ? kSettingsMinusX : kSettingsPlusX;
            EpdRect r = {bx, y + (kMenuRowH - kSettingsBtn) / 2, kSettingsBtn,
                         kSettingsBtn};
            epd_draw_rect(r, 0x00, fb);
            EpdRect r2 = {r.x + 1, r.y + 1, r.width - 2, r.height - 2};
            epd_draw_rect(r2, 0x00, fb);
            int cx = r.x + r.width / 2, cy = r.y + r.height / 2;
            epd_fill_rect({cx - 12, cy - 2, 24, 5}, 0x00, fb);
            if (b == 1) epd_fill_rect({cx - 2, cy - 12, 5, 24}, 0x00, fb);
        }
        int vx = (kSettingsMinusX + kSettingsBtn + kSettingsPlusX) / 2;
        ui::text(&ArialBold_20, vx, y + 88, rows[i].value, fb,
                 EPD_DRAW_ALIGN_CENTER);
        epd_fill_rect({0, y + kMenuRowH - 1, W, 1}, 0x80, fb);
    }

    // Navigation rows below the two adjustable settings.
    struct NavRow { const char* title; const char* sub; };
    const NavRow navs[2] = {
        {"Sensors", "pair heart rate / power / cadence"},
        {"GPS Debug", "receiver diagnostics"},
    };
    for (int i = 0; i < 2; ++i) {
        int y = kMenuRowTop + (3 + i) * kMenuRowH;
        ui::text(&ArialBold_20, 28, y + 64, navs[i].title, fb);
        ui::text(&ArialBold_14, 28, y + 106, navs[i].sub, fb,
                 EPD_DRAW_ALIGN_LEFT, 0x60);
        ui::text(&ArialBold_20, W - 28, y + 84, ">", fb, EPD_DRAW_ALIGN_RIGHT);
        epd_fill_rect({0, y + kMenuRowH - 1, W, 1}, 0x80, fb);
    }

    ui::text(&ArialBold_14, W / 2, kBackBar.y - 16,
             FIRMWARE_VERSION " · more settings in src/config.h", fb,
             EPD_DRAW_ALIGN_CENTER, 0x60);
    ui_render_back_bar(fb);
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

    DBG_LINE("MODULE", "%s", g.moduleDetected ? "detected" : "NOT DETECTED");
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
        DBG_LINE("ALT / SPEED", "%.0f m · %.1f km/h", g.altM, g.speedKmh);
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
             fb, EPD_DRAW_ALIGN_LEFT, 0x60);

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
    ui::label(W / 2, 440, "POWERED OFF", fb, 0x00, &ArialBold_20);
    ui::text(&ArialBold_14, W / 2, 500, "press the BOOT button to wake", fb,
             EPD_DRAW_ALIGN_CENTER, 0x60);
}

void ui_render_nav_prompt(const char* routeName, int turns, uint8_t* fb) {
    const int W = epd_rotated_display_width();

    epd_fill_rect({kPowerSheet.x, kPowerSheet.y, kPowerSheet.width,
                   kPowerSheet.height}, 0xFF, fb);
    epd_fill_rect({0, kPowerSheet.y, W, 4}, 0x00, fb);

    // Signpost glyph
    int gx = 70, gy = kPowerSheet.y + 60;
    epd_fill_rect({gx - 4, gy - 34, 8, 60}, 0x00, fb);
    epd_fill_triangle(gx + 34, gy - 22, gx - 2, gy - 34, gx - 2, gy - 10, 0x00, fb);

    ui::text(&ArialBold_20, 118, kPowerSheet.y + 54, "Start navigation?", fb);
    char sub[64];
    snprintf(sub, sizeof(sub), "%s · %d turns", routeName, turns);
    ui::text(&ArialBold_14, 118, kPowerSheet.y + 94, sub, fb,
             EPD_DRAW_ALIGN_LEFT, 0x60);

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
                          uint8_t* fb) {
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
    if (distanceM >= 1000) snprintf(d, sizeof(d), "%.1f km", distanceM / 1000);
    else snprintf(d, sizeof(d), "%d m", (int)(distanceM + 0.5f));
    ui::text(&ArialBold_20, 108, top + 52, d, fb, EPD_DRAW_ALIGN_LEFT, 0xFF);
    ui::text(&ArialBold_14, 24, top + 112, instruction, fb,
             EPD_DRAW_ALIGN_LEFT, 0xFF);
}
