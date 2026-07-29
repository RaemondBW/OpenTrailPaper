// Standalone evaluation of the EPD_Painter driver on the LilyGO T5S3 4.7" PRO.
//
// WHY: our own epdiy-based renderer suffers a slow grey cast that creeps into
// the white background while the device sits there updating (see
// investigations/display-ghosting.md). The mechanism is DC imbalance — DU is a
// fast 1-bit waveform that is not DC-balanced, so pixels that hold still are
// still driven by every scan, and the imbalance accumulates. EPD_Painter claims
// to address exactly this: it has an explicit DC-balance pass (unpaintPacked),
// a delta engine, and per-panel measured waveform tables for THIS board.
//
// This app does NOT touch the shipping firmware. It reproduces the conditions
// that cause our drift and lets us watch the panel, so we can decide whether a
// full port (a ~1400-line rewrite of ui_render/map_view onto Adafruit GFX) is
// justified. Build with: pio run -e epdpainter-test -t upload
//
// The board preset matches our hardware exactly — verified against
// vendor/.../epd_board_v7.c: data pins {5,6,7,15,16,17,18,8}, CKV 48, SPH 41,
// LE 42, I2C SDA 39 / SCL 40, TPS65185 @0x68 + PCA9555 @0x20.
#define EPD_PAINTER_PRESET_LILYGO_T5_S3_GPS

#include <Arduino.h>

#include "EPD_Painter_presets.h"
#include "EPD_Painter.h"
#include "EPD_Painter_Adafruit.h"

// Portrait, to match the firmware's 540x960 orientation.
static EPD_PainterAdafruit gfx(EPD_LILYGO_T5_S3_GPS_PRESET, /*portrait=*/true);

static const int W = 540;
static const int H = 960;

// Greys as GFXcanvas8 values: 0 = black, 255 = white.
static const uint8_t BLACK = 0x00;
static const uint8_t WHITE = 0xFF;

// A screentone fill, the same idea the map uses for water/parks: 1-bit patterns
// rather than a grey value, because a true grey is not displayable on the fast
// refresh. 75% dots and a 2px-pitch diagonal hatch.
static void screentone(int x0, int y0, int w, int h, bool hatch) {
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) {
            const bool on = hatch ? (((x - y) & 3) < 2)
                                  : !((x & 1) && (y & 1));
            if (on) gfx.drawPixel(x, y, BLACK);
        }
    }
}

// The static part of the mock dashboard: everything that will NOT change
// between updates. This is the region we care about — if it greys over time,
// the driver has the same DC-balance problem ours does.
static void drawChrome() {
    gfx.fillScreen(WHITE);

    // Inverted status band (the heaviest black area in our real UI).
    gfx.fillRect(0, 0, W, 64, BLACK);
    gfx.setTextColor(WHITE);
    gfx.setTextSize(2);
    gfx.setCursor(12, 22);
    gfx.print("EPD_PAINTER DRIFT TEST");

    gfx.setTextColor(BLACK);

    // Large white field, deliberately empty. THIS is what we watch: it must
    // stay white while the counter below updates once a second.
    gfx.drawRect(20, 90, W - 40, 210, BLACK);
    gfx.setTextSize(2);
    gfx.setCursor(34, 104);
    gfx.print("WATCH THIS WHITE AREA");

    // Screentone swatches, to compare how the driver renders our dither
    // patterns versus the flat-grey look we were getting.
    gfx.setCursor(34, 320);
    gfx.print("SCREENTONES");
    screentone(20, 344, (W - 60) / 2, 120, false);   // 75% dots  (water)
    screentone(20 + (W - 40) / 2, 344, (W - 60) / 2, 120, true);  // hatch (parks)

    // Some body text, to see how 1-bit GFX fonts read next to our
    // anti-aliased epdiy fonts.
    gfx.setTextSize(1);
    gfx.setCursor(24, 490);
    gfx.print("Counter below updates 1 Hz - a partial/delta update.");
    gfx.setCursor(24, 506);
    gfx.print("That repeated update is what greys our panel today.");
}

void setup() {
    Serial.begin(115200);
    delay(400);
    Serial.println("\n[epdtest] EPD_Painter evaluation starting");

    if (!gfx.begin()) {
        Serial.println("[epdtest] gfx.begin() FAILED - wrong preset or no PSRAM?");
        return;
    }
    Serial.println("[epdtest] begin() ok");

    // Start from a genuinely clean panel so any grey we see afterwards was
    // produced during this run rather than inherited.
    gfx.clear();
    Serial.println("[epdtest] panel cleared");

    drawChrome();
    gfx.paint();
    Serial.println("[epdtest] first full paint done");
}

void loop() {
    static uint32_t n = 0;
    static uint32_t lastDcMs = 0;

    // Redraw ONLY the counter, then delta-paint. This is the analogue of our
    // clock/GPS-dot updates: a tiny change, once a second, forever.
    char buf[24];
    snprintf(buf, sizeof(buf), "%lu s", (unsigned long)n);

    gfx.fillRect(24, 540, W - 48, 90, WHITE);
    gfx.setTextColor(BLACK);
    gfx.setTextSize(5);
    gfx.setCursor(24, 556);
    gfx.print(buf);

    gfx.paint();   // delta update: only changed pixels should be driven

    // Every 2 minutes, report so a long unattended soak is diagnosable, and
    // exercise the driver's own dirty-area clean. If the white field stays
    // white WITHOUT this, the delta engine alone is enough; if it only stays
    // white with it, we know the periodic clean is doing the work.
    if (millis() - lastDcMs > 120000) {
        lastDcMs = millis();
        Serial.printf("[epdtest] %lu updates, uptime %lus, heap %u\n",
                      (unsigned long)n, (unsigned long)(millis() / 1000),
                      (unsigned)ESP.getFreeHeap());
    }

    n++;
    delay(1000);
}
