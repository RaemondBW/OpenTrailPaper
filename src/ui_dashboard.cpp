#include "ui_dashboard.h"

#include <Arduino.h>
#include <SD.h>
#include <Wire.h>
#include <epdiy.h>
#include <TouchDrvGT911.hpp>

#include "config.h"
#include "ride_state.h"
#include "ride_recorder.h"
#include "gps_service.h"
#include "board_power.h"
#include <esp_sleep.h>
#include <driver/gpio.h>
#include "ui_render.h"
#include "map_view.h"
#include "map_tiles.h"
#include "ble_sensors.h"
#include "ble_server.h"
#include "routes.h"
#include "settings.h"
#include "diag.h"

// SF map embedded in flash (board_build.embed_files = data/sf.ebm)
extern const uint8_t map_ebm_start[] asm("_binary_data_sf_ebm_start");
extern const uint8_t map_ebm_end[] asm("_binary_data_sf_ebm_end");

namespace {

EpdiyHighlevelState hl;
TouchDrvGT911 touch;
bool touchOk = false;
int refreshCount = 0;

// Shadow of the last frame actually pushed to glass. The panel must not
// repaint when nothing changed (design: "static chrome never repaints"),
// so identical frames are dropped before any epd update.
uint8_t* shadowFb = nullptr;
size_t fbSize = 0;

enum Screen { SCREEN_DASH, SCREEN_MAP, SCREEN_SUMMARY, SCREEN_MENU,
              SCREEN_SENSORS, SCREEN_ROUTES, SCREEN_HISTORY,
              SCREEN_SETTINGS, SCREEN_GPSDEBUG };
Screen screen = SCREEN_DASH;
RideSummary pendingSummary;

float mapMpp = 2.0f;  // map zoom: 1/2/4/8 m per px
bool mapTrackUp = false;

// When the "Start navigation?" prompt appeared (for the accept settle guard).
uint32_t navPromptShownAt = 0;

// Turn-by-turn banner rect (top of map/dashboard); tapping it ends nav.
const EpdRect kNavBanner = {0, ui::STATUS_H, 540, 138};
void drawNavBanner(uint8_t* fb);
void buildMapScreenData(const RideState& s, MapScreenData& map);

// Which screen the Sensors page was opened from, so back returns there.
Screen sensorsFrom = SCREEN_MENU;

// Set from the GT911 home-button callback (fires inside touch.getPoint()).
volatile bool homeKeyPressed = false;

// Interrupt flags. ISRs only set these; all I2C reads and logic stay in the
// UI task. A slow fallback poll (see the task loop) covers the case where an
// INT line doesn't behave as expected, so input can never be dropped.
volatile bool touchIrq = false;      // GT911 INT (GPIO3)
volatile bool boardBtnIrq = false;   // BOOT edge or expander INT (GPIO38)

void IRAM_ATTR onTouchIrq() { touchIrq = true; }
void IRAM_ATTR onBoardBtnIrq() { boardBtnIrq = true; }

// Power/shutdown dialog overlay (opened by holding BOOT 1.5 s).
bool powerOverlay = false;

// Frontlight: 4 levels cycled by the GPIO48 button.
const uint8_t kBacklightPWM[4] = {0, 50, 110, 230};
void applyBacklight(int level) {
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    analogWrite(BOARD_BL_EN, kBacklightPWM[level]);
}

void shutdownDevice(uint8_t* fb) {
    Serial.println("[power] shutting down");
    if (ride_recorder::isRecording()) {
        ride_recorder::stopRide(true);  // never lose a ride to power-off
    }

    // Leave a static farewell on the glass — e-paper keeps it for free.
    // Full-screen map backdrop (last known position) with a POWERED OFF plate.
    memset(fb, 0xFF, fbSize);
    {
        RideState s = g_state.snapshot();
        MapScreenData map = {};
        buildMapScreenData(s, map);
        ui_render_map_features(map, s, fb);
    }
    ui_render_shutdown_screen(fb);
    if (shadowFb) memcpy(shadowFb, fb, fbSize);
    epd_poweron();
    epd_fullclear(&hl, epd_ambient_temperature());
    if (shadowFb) memcpy(fb, shadowFb, fbSize);  // fullclear wipes fb
    epd_hl_update_screen(&hl, MODE_GC16, epd_ambient_temperature());
    epd_poweroff();

    // Peripherals down, matching the factory sleep sequence
    touch.sleep();
    digitalWrite(BOARD_TOUCH_RST, LOW);
    gpio_hold_en((gpio_num_t)BOARD_TOUCH_RST);
    gpio_deep_sleep_hold_en();
    board_radio_power(false);

    esp_sleep_enable_ext0_wakeup((gpio_num_t)BOARD_BOOT_BTN, 0);
    esp_deep_sleep_start();
}

// Screen-space route overlay (rebuilt each map render)
constexpr int MAX_ROUTE_SCREEN_PTS = 4096;
int16_t* routeScreenPts = nullptr;

// Rows behind the list screens, rebuilt on entry/render
ble_sensors::Candidate sensorCands[8];
int sensorCandCount = 0;
char routeFiles[5][routes::MAX_NAME];
int routeFileCount = 0;

// Long-press tracking
uint32_t touchDownAt = 0;
bool touchWasDown = false;

// Inactivity auto-sleep: any input refreshes this; the task deep-sleeps after
// a quiet period (unless riding / navigating / phone connected).
uint32_t lastActivityMs = 0;
constexpr uint32_t AUTO_SLEEP_MS = 10 * 60 * 1000;   // 10 minutes idle
void noteActivity() {
    lastActivityMs = millis();
    ble_sensors::noteActivity();
}

// Hybrid refresh policy to avoid the GC16 white-flash on every page.
//
//   - Page transition  -> GL16: renders full grays, NO inversion flash.
//   - In-page update on a pure black/white screen -> DU: fast 1-bit.
//   - Deep clean (GC16 + white flash) only when ghosting has actually
//     built up, tracked by `ghostDebt`, not on a fixed page count.
//
// DU leaves ghosting and can't render gray, so it is used only for the
// live black/white screens; everything else (and every transition) takes
// the clean GL16 path.
// Refresh strategy that never does the slow, flashing GC16 clean during
// use:
//   - Live black/white screen  -> DU (fast, ~300 ms), accumulates ghosting.
//   - Every GHOST_GL16_EVERY DU updates -> one GL16 instead: a full but
//     NON-flashing pass that clears accumulated ghosting.
//   - Page transitions / gray screens -> GL16 (no flash, moderate speed).
// GC16 (the black/white/black flash) only ever runs at boot and shutdown.
int ghostDebt = 0;
constexpr int GHOST_GL16_EVERY = 24;   // ~24 s of DU before a clean pass

bool refresh(bool screenChanged, bool fastOk) {
    uint8_t* fb = epd_hl_get_framebuffer(&hl);
    if (!screenChanged && shadowFb && memcmp(fb, shadowFb, fbSize) == 0) {
        return false;  // identical frame — never touch the panel
    }
    if (shadowFb) memcpy(shadowFb, fb, fbSize);

    epd_poweron();
    if (fastOk && !screenChanged && ghostDebt < GHOST_GL16_EVERY) {
        // Fast partial update; the highlevel diff only rewrites changed
        // pixels, so this is cheap for live numbers.
        epd_hl_update_screen(&hl, MODE_DU, epd_ambient_temperature());
        ghostDebt += 1;
    } else {
        // Non-flashing full-quality pass: page changes, gray screens, and
        // the periodic ghost clean.
        epd_hl_update_screen(&hl, MODE_GL16, epd_ambient_temperature());
        ghostDebt = 0;
    }
    epd_poweroff();
    refreshCount++;
    return true;
}

// Screens that are pure black/white AND update in place can use the fast
// DU path between frames. Screens with gray tones must take GL16.
bool screenIsFast(Screen s, bool overlay) {
    if (overlay) return false;  // power sheet has gray subtitle
    return s == SCREEN_DASH || s == SCREEN_MAP || s == SCREEN_GPSDEBUG;
}

bool inRect(const EpdRect& r, int x, int y) {
    return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
}

void enterSensors();
void leaveList();
void handlePowerTap(int x, int y);

// Navigate up one level in the screen hierarchy (front button, short press).
void goBack() {
    if (routes::navPending()) {
        routes::dismissNav();
        return;
    }
    if (powerOverlay) {
        powerOverlay = false;
        return;
    }
    switch (screen) {
        case SCREEN_GPSDEBUG: screen = SCREEN_SETTINGS; break;
        case SCREEN_SETTINGS:
        case SCREEN_ROUTES:
        case SCREEN_HISTORY:  screen = SCREEN_MENU; break;
        case SCREEN_SENSORS:  leaveList(); break;  // also stops the BLE scan
        case SCREEN_MENU:     screen = SCREEN_DASH; break;
        case SCREEN_MAP:      screen = SCREEN_DASH; break;
        // The summary needs an explicit SAVE / DISCARD, so back is ignored.
        case SCREEN_SUMMARY:  break;
        case SCREEN_DASH:     break;  // already home
    }
}

// BOOT short-press: start a ride, or stop it (via the save/discard summary).
void toggleRide() {
    if (ride_recorder::isRecording()) {
        pendingSummary = ride_recorder::summary();
        screen = SCREEN_SUMMARY;
    } else {
        ride_recorder::startRide();
        screen = SCREEN_DASH;
    }
}

// Side-key short-press: step the frontlight Off -> Low -> Med -> Bright -> Off.
void cycleFrontlight() {
    int next = (settings::backlight() + 1) & 3;
    settings::setBacklight(next);
    applyBacklight(next);
}

void handleTap(int x, int y) {
    // The "Start navigation?" prompt owns every tap while it is up. START
    // begins navigation; any other tap dismisses it, so it can never trap
    // the UI.
    if (routes::navPending()) {
        // Ignore taps for a moment after the prompt appears so a touch that
        // was already in progress when the route arrived can't auto-accept
        // it. The user must deliberately tap START.
        if (millis() - navPromptShownAt < 700) return;
        if (inRect(kPowerShutdown, x, y)) {
            routes::startNav();
            screen = SCREEN_MAP;
        } else {
            routes::dismissNav();
        }
        return;
    }

    // Bottom BACK strip on every sub-screen.
    if ((screen == SCREEN_SETTINGS || screen == SCREEN_SENSORS ||
         screen == SCREEN_ROUTES || screen == SCREEN_HISTORY ||
         screen == SCREEN_GPSDEBUG) &&
        inRect(kBackBar, x, y)) {
        goBack();
        return;
    }

    // Tapping the turn banner (map or dashboard) ends navigation.
    if (routes::navActive() && inRect(kNavBanner, x, y) &&
        (screen == SCREEN_DASH || screen == SCREEN_MAP)) {
        routes::dismissNav();
        return;
    }

    switch (screen) {
        case SCREEN_DASH:
        case SCREEN_MAP:
            if (screen == SCREEN_MAP) {
                int dx = x - kMapCompass.cx, dy = y - kMapCompass.cy;
                if (dx * dx + dy * dy <= kMapCompass.r * kMapCompass.r) {
                    mapTrackUp = !mapTrackUp;
                    break;
                }
            }
            if (screen == SCREEN_MAP && x >= kMapZoom.zoomX &&
                x < kMapZoom.zoomX + kMapZoom.size) {
                if (y >= kMapZoom.zoomInY && y < kMapZoom.zoomInY + kMapZoom.size) {
                    if (mapMpp > 1.0f) mapMpp /= 2.0f;
                    break;
                }
                if (y >= kMapZoom.zoomOutY && y < kMapZoom.zoomOutY + kMapZoom.size) {
                    if (mapMpp < 8.0f) mapMpp *= 2.0f;
                    break;
                }
            }
            // Status bar opens the menu; anywhere else flips dash <-> map.
            if (y < ui::STATUS_H) screen = SCREEN_MENU;
            else screen = screen == SCREEN_DASH ? SCREEN_MAP : SCREEN_DASH;
            break;
        case SCREEN_SUMMARY:
            if (inRect(kSaveButton, x, y)) {
                ride_recorder::stopRide(true);
                screen = SCREEN_DASH;
            } else if (inRect(kDiscardButton, x, y)) {
                ride_recorder::stopRide(false);
                screen = SCREEN_DASH;
            }
            break;
        case SCREEN_MENU: {
            int row = (y - kMenuRowTop) / kMenuRowH;
            if (y >= kMenuRowTop && row >= 0 && row < kMenuRowCount) {
                switch (row) {
                    case 0:
                        if (ride_recorder::isRecording()) {
                            pendingSummary = ride_recorder::summary();
                            screen = SCREEN_SUMMARY;
                        } else {
                            ride_recorder::startRide();
                            screen = SCREEN_DASH;
                        }
                        break;
                    case 1: screen = SCREEN_ROUTES; break;
                    case 2: sensorsFrom = SCREEN_MENU; enterSensors(); break;
                    case 3: screen = SCREEN_HISTORY; break;
                    case 4: screen = SCREEN_SETTINGS; break;
                }
            } else {
                screen = SCREEN_DASH;  // header or footer: back to ride
            }
            break;
        }
        case SCREEN_SENSORS: {
            int row = (y - kMenuRowTop) / kMenuRowH;
            if (y >= kMenuRowTop && row >= 0 && row < sensorCandCount &&
                row < kMenuRowCount) {
                ble_sensors::pairCandidate(sensorCands[row].addr);
            } else {
                leaveList();
            }
            break;
        }
        case SCREEN_ROUTES: {
            int row = (y - kMenuRowTop) / kMenuRowH;
            int base = routes::active() ? 1 : 0;
            if (y >= kMenuRowTop && row >= 0 && row < kMenuRowCount) {
                if (routes::active() && row == 0) {
                    routes::clearRoute();
                } else if (row - base < routeFileCount && row - base >= 0) {
                    if (routes::load(routeFiles[row - base])) {
                        screen = SCREEN_MAP;
                    }
                }
            } else {
                screen = SCREEN_MENU;
            }
            break;
        }
        case SCREEN_HISTORY:
            screen = SCREEN_MENU;
            break;
        case SCREEN_SETTINGS: {
            int row = (y - kMenuRowTop) / kSettingsRowH;
            bool minus = x >= kSettingsMinusX && x < kSettingsMinusX + kSettingsBtn;
            bool plus = x >= kSettingsPlusX && x < kSettingsPlusX + kSettingsBtn;
            if (y >= kMenuRowTop && row >= 0 && row < 4 && (minus || plus)) {
                int dir = plus ? 1 : -1;
                if (row == 0) settings::setFtpWatts(settings::ftpWatts() + dir * 5);
                if (row == 1) settings::setTzMinutes(settings::tzMinutes() + dir * 30);
                if (row == kSettingsBacklightRow) {
                    settings::setBacklight(settings::backlight() + dir);
                    applyBacklight(settings::backlight());
                }
                if (row == kSettingsUnitsRow) {   // either button toggles
                    settings::setUseMiles(!settings::useMiles());
                }
                g_state.with([](RideState& st) {
                    st.ftpW = (uint16_t)settings::ftpWatts();
                    st.tzMin = (int16_t)settings::tzMinutes();
                    st.useMiles = settings::useMiles();
                });
                ble_server::pushSettingsToPhone();   // mirror the edit to the app
            } else if (y >= kMenuRowTop && row == kSettingsSensorsRow) {
                sensorsFrom = SCREEN_SETTINGS;
                enterSensors();
            } else if (y >= kMenuRowTop && row == kSettingsGpsRow) {
                screen = SCREEN_GPSDEBUG;
            } else {
                screen = SCREEN_MENU;
            }
            break;
        }
        case SCREEN_GPSDEBUG:
            screen = SCREEN_SETTINGS;
            break;
    }
}

void handlePowerTap(int x, int y) {
    if (inRect(kPowerShutdown, x, y)) {
        uint8_t* fb = epd_hl_get_framebuffer(&hl);
        shutdownDevice(fb);  // does not return
    } else {
        // CANCEL, or a tap anywhere outside the sheet, dismisses it.
        powerOverlay = false;
    }
}

void buildMapScreenData(const RideState& s, MapScreenData& map) {
    map.riderX = 270;
    map.riderY = 430;
    // Track-up: the world rotates so travel direction is up; the rider
    // arrow points up and the compass needle shows where north went.
    float rot = mapTrackUp ? -s.courseDeg : 0.0f;
    map.headingDeg = mapTrackUp ? 0.0f : s.courseDeg;
    map.northDeg = rot;
    map.trackUp = mapTrackUp;
    map.metersPerPixel = mapMpp;

    // Hold the last known position through fix dropouts; fall back to the
    // persisted position from a previous session, then the default. Never
    // jump to the default mid-ride.
    double lat = DEFAULT_MAP_LAT, lon = DEFAULT_MAP_LON;
    if (s.everHadFix) {
        lat = s.latitude;
        lon = s.longitude;
    } else {
        settings::lastPosition(lat, lon);
    }
    map_tiles::project(lat, lon, mapMpp, map.riderX, map.riderY, rot, map);

    if (routes::active() && routeScreenPts) {
        int n = routes::pointCount();
        if (n > MAX_ROUTE_SCREEN_PTS) n = MAX_ROUTE_SCREEN_PTS;
        for (int i = 0; i < n; ++i) {
            double plat, plon;
            routes::point(i, plat, plon);
            map_tiles::geoToScreen(plat, plon, lat, lon, mapMpp, map.riderX,
                                   map.riderY, rot, routeScreenPts[i * 2],
                                   routeScreenPts[i * 2 + 1]);
        }
        map.route = routeScreenPts;
        map.routePointCount = n;
        map.riddenPointCount = routes::progressIndex() + 1;
        map.showRemaining = true;
        map.remainingKm = routes::remainingKm();
    }
}

void renderMapScreen(const RideState& s, uint8_t* fb) {
    MapScreenData map = {};
    buildMapScreenData(s, map);
    ui_render_map(map, s, fb);
    drawNavBanner(fb);
}

// Turn-by-turn banner across the top, shown on the map AND the dashboard
// while navigating. Tapping it (kNavBanner) ends navigation.
void drawNavBanner(uint8_t* fb) {
    if (!routes::navActive()) return;
    char instr[routes::MANEUVER_TEXT];
    float dist = 0;
    if (routes::nextTurn(instr, sizeof(instr), dist)) {
        ui_render_nav_banner(instr, dist, settings::useMiles(), fb);
    }
}

void enterSensors() {
    ble_sensors::setScanAlways(true);
    screen = SCREEN_SENSORS;
}

void leaveList() {
    ble_sensors::setScanAlways(false);
    screen = sensorsFrom;  // Menu or Settings, whichever opened it
}

const char* kindsText(uint8_t mask) {
    switch (mask & 0x7) {
        case 1: return "heart rate";
        case 2: return "power";
        case 3: return "HR + power";
        case 4: return "cadence";
        case 6: return "power + cadence";
        case 7: return "HR + power + cadence";
        default: return "sensor";
    }
}

void renderListScreen(uint8_t* fb) {
    ListRow rows[kMenuRowCount] = {};
    int count = 0;
    const char* title = "";
    const char* footer = "";

    switch (screen) {
        case SCREEN_SENSORS: {
            title = "SENSORS";
            footer = "tap a sensor to pair it · scanning...";
            sensorCandCount = ble_sensors::getCandidates(sensorCands, 8);
            count = sensorCandCount < kMenuRowCount ? sensorCandCount
                                                    : kMenuRowCount;
            for (int i = 0; i < count; ++i) {
                auto& c = sensorCands[i];
                snprintf(rows[i].title, sizeof(rows[i].title), "%s",
                         c.name[0] ? c.name : c.addr);
                snprintf(rows[i].subtitle, sizeof(rows[i].subtitle),
                         "%s · %d dBm%s", kindsText(c.kindsMask), c.rssi,
                         c.connected ? " · connected"
                                     : (c.paired ? " · paired" : ""));
                rows[i].inverted = c.connected;
            }
            break;
        }
        case SCREEN_ROUTES: {
            title = "NAVIGATE";
            footer = ride_recorder::sdMounted()
                         ? "put .gpx files in /routes on the SD card"
                         : "no SD card";
            int base = 0;
            if (routes::active()) {
                snprintf(rows[0].title, sizeof(rows[0].title), "Clear route");
                snprintf(rows[0].subtitle, sizeof(rows[0].subtitle),
                         "%s · %.1f %s left", routes::activeName(),
                         units::dist(routes::remainingKm(), settings::useMiles()),
                         settings::useMiles() ? "mi" : "km");
                rows[0].inverted = true;
                base = 1;
            }
            routeFileCount = routes::list(routeFiles, 5 - base);
            count = base + routeFileCount;
            for (int i = 0; i < routeFileCount; ++i) {
                snprintf(rows[base + i].title, sizeof(rows[0].title), "%s",
                         routeFiles[i]);
                snprintf(rows[base + i].subtitle, sizeof(rows[0].subtitle),
                         "tap to ride this route");
            }
            break;
        }
        case SCREEN_HISTORY: {
            title = "RIDE HISTORY";
            footer = "rides upload from /rides on the SD card";
            File dir = SD.open(RIDE_DIR);
            if (dir) {
                for (File f = dir.openNextFile(); f && count < kMenuRowCount;
                     f = dir.openNextFile()) {
                    if (!f.isDirectory()) {
                        const char* base = strrchr(f.name(), '/');
                        base = base ? base + 1 : f.name();
                        snprintf(rows[count].title, sizeof(rows[0].title),
                                 "%s", base);
                        snprintf(rows[count].subtitle, sizeof(rows[0].subtitle),
                                 "%lu KB", (unsigned long)(f.size() / 1024));
                        count++;
                    }
                    f.close();
                }
                dir.close();
            }
            break;
        }
        default:
            break;
    }
    ui_render_list(title, rows, count, footer, fb);
}

}  // namespace

namespace ui_dashboard {

bool begin() {
    epd_init(&epd_board_v7, &ED047TC1, EPD_LUT_64K);
    epd_set_vcom(1560);
    hl = epd_hl_init(EPD_BUILTIN_WAVEFORM);
    epd_set_rotation(DISPLAY_ROTATION);

    // Frontlight — level set in Settings, applied here at boot.
    pinMode(BOARD_BL_EN, OUTPUT);
    applyBacklight(settings::backlight());

    // Deep-sleep shutdown latches the touch RST pin LOW (gpio_hold) to keep
    // the GT911 in reset. Release that hold on boot, or after a wake the
    // controller stays reset and touch never works (can't reach map/settings).
    gpio_hold_dis((gpio_num_t)BOARD_TOUCH_RST);
    gpio_deep_sleep_hold_dis();

    touch.setPins(BOARD_TOUCH_RST, BOARD_TOUCH_INT);
    touchOk = touch.begin(Wire, GT911_SLAVE_ADDRESS_L, BOARD_SDA, BOARD_SCL);
    if (!touchOk) Serial.println("[ui] GT911 touch not found");

    // Interrupt-drive the inputs. The GT911 pulses its INT on a touch event;
    // the XL9555 pulls its INT low when a button changes; BOOT is a plain
    // GPIO edge. ISRs just set a flag that the task acts on.
    attachInterrupt(digitalPinToInterrupt(BOARD_TOUCH_INT), onTouchIrq, FALLING);
    attachInterrupt(digitalPinToInterrupt(BOARD_BOOT_BTN), onBoardBtnIrq, CHANGE);
    pinMode(BOARD_PCA9535_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BOARD_PCA9535_INT), onBoardBtnIrq,
                    FALLING);

    // The capacitive Home button below the display is a GT911 key, reported
    // via this callback during touch.getPoint(). It navigates back.
    touch.setHomeButtonCallback([](void*) { homeKeyPressed = true; }, nullptr);

    epd_poweron();
    epd_clear();
    epd_poweroff();

    fbSize = epd_width() / 2 * epd_height();
    shadowFb = (uint8_t*)heap_caps_malloc(fbSize, MALLOC_CAP_SPIRAM);

    routeScreenPts = (int16_t*)heap_caps_malloc(
        MAX_ROUTE_SCREEN_PTS * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);

    size_t mapLen = map_ebm_end - map_ebm_start;
    if (map_tiles::load(map_ebm_start, mapLen)) {
        Serial.printf("[ui] map loaded: %u KB\n", (unsigned)(mapLen / 1024));
    } else {
        Serial.println("[ui] embedded map failed to load");
    }
    return true;
}

void task(void*) {
    uint32_t lastDraw = 0;
    Screen lastScreen = screen;
    bool lastOverlay = false;
    bool lastNavPrompt = false;

    lastActivityMs = millis();
    for (;;) {
        // Auto-sleep after a quiet period to save battery — a bike computer
        // left on a desk otherwise burns power on GPS + BLE + CPU. Held off
        // while recording, navigating, or a phone is connected. Wake with BOOT.
        if (millis() - lastActivityMs > AUTO_SLEEP_MS &&
            !ride_recorder::isRecording() && !routes::navActive() &&
            !ble_server::isPhoneConnected()) {
            diag::log("auto-sleep after %lu min idle", AUTO_SLEEP_MS / 60000);
            uint8_t* fb = epd_hl_get_framebuffer(&hl);
            shutdownDevice(fb);   // deep sleep; does not return
        }

        // Apply a frontlight level changed from the phone (BLE writes NVS but
        // not the PWM). Also covers the device's own +/- which already applied.
        static int lastBl = -1;
        if (settings::backlight() != lastBl) {
            lastBl = settings::backlight();
            applyBacklight(lastBl);
        }

        // Left-side physical buttons, each with its own debounce/hold state.
        // BOOT (GPIO0): short press starts/stops the ride, hold 1.5 s opens the
        // power dialog. Side key (expander PC12): short press cycles the
        // frontlight. Interrupt-driven, with a 200 ms fallback poll; two
        // consecutive LOW reads debounce noise.
        {
            bool irq = boardBtnIrq;
            boardBtnIrq = false;

            static int bootLow = 0;
            static uint32_t bootDownAt = 0, bootPoll = 0;
            static bool bootLong = false;
            if (irq || bootLow > 0 || millis() - bootPoll > 200) {
                bootPoll = millis();
                if (digitalRead(BOARD_BOOT_BTN) == LOW) {
                    if (bootLow < 200) bootLow++;
                    if (bootLow == 2) {            // debounced press start
                        bootDownAt = millis();
                        bootLong = false;
                    } else if (bootLow > 2 && !bootLong && !powerOverlay &&
                               millis() - bootDownAt > 1500) {
                        bootLong = true;
                        powerOverlay = true;       // hold -> power dialog
                    }
                } else {
                    if (bootLow >= 2 && !bootLong) { noteActivity(); toggleRide(); }
                    bootLow = 0;
                }
            }

            static int sideLow = 0;
            static uint32_t sidePoll = 0;
            if (irq || sideLow > 0 || millis() - sidePoll > 200) {
                sidePoll = millis();
                if (board_side_button_pressed()) {
                    if (sideLow < 200) sideLow++;
                } else {
                    if (sideLow >= 2) { noteActivity(); cycleFrontlight(); }
                    sideLow = 0;
                }
            }
        }

        // Touch: interrupt-driven. getPoint() (an I2C read) runs when the
        // GT911 INT fired, while a touch is ongoing (to catch the release),
        // or on a slow fallback tick so input is never dropped.
        if (touchOk) {
            static int16_t lastX = 0, lastY = 0;
            static uint32_t lastTouchPoll = 0;
            if (touchIrq || touchWasDown || millis() - lastTouchPoll > 200) {
                touchIrq = false;
                lastTouchPoll = millis();
                int16_t x[1], y[1];
                bool down = touch.getPoint(x, y, 1) > 0;
                if (down) {
                    lastX = x[0];
                    lastY = y[0];
                }
                static uint32_t lastTapMs = 0;
                if (down && !touchWasDown) {
                    touchDownAt = millis();
                } else if (!down && touchWasDown) {
                    // One tap per release, debounced so a flickery touch read
                    // can't fire twice and toggle the screen back.
                    if (millis() - lastTapMs > 350) {
                        lastTapMs = millis();
                        noteActivity();
                        if (powerOverlay) handlePowerTap(lastX, lastY);
                        else handleTap(lastX, lastY);
                    }
                }
                touchWasDown = down;
            }
        }

        // Capacitive Home button (GT911 key) — navigate back. Debounced so
        // one press triggers once even while the key is held.
        if (homeKeyPressed) {
            homeKeyPressed = false;
            static uint32_t lastHome = 0;
            if (millis() - lastHome > 400) {
                lastHome = millis();
                noteActivity();
                goBack();
            }
        }

        bool navPrompt = routes::navPending();
        if (navPrompt && !lastNavPrompt) navPromptShownAt = millis();
        bool screenChanged = screen != lastScreen || powerOverlay != lastOverlay
                             || navPrompt != lastNavPrompt;
        if (screenChanged || millis() - lastDraw >= 1000) {
            lastDraw = millis();
            lastScreen = screen;
            lastOverlay = powerOverlay;
            lastNavPrompt = navPrompt;
            RideState s = g_state.snapshot();
            uint8_t* fb = epd_hl_get_framebuffer(&hl);
            memset(fb, 0xFF, epd_width() / 2 * epd_height());
            switch (screen) {
                case SCREEN_DASH:
                    ui_render_dashboard(s, routes::navActive(), fb);
                    drawNavBanner(fb);  // turn cue on the data page too
                    break;
                case SCREEN_MAP: renderMapScreen(s, fb); break;
                case SCREEN_SUMMARY: ui_render_summary(pendingSummary, fb); break;
                case SCREEN_MENU: {
                    MenuInfo m;
                    m.recording = s.recording;
                    m.gpsReady = s.gpsFix && s.timeValid;
                    m.sdOk = ride_recorder::sdMounted();
                    m.rideCount = ride_recorder::rideCount();
                    m.sdFreeMB = ride_recorder::sdFreeMB();
                    m.hr = s.hrConnected;
                    m.pwr = s.powerConnected;
                    m.cad = s.cadenceConnected;
                    m.batteryPercent = s.batteryPercent;
                    m.rideDistanceM = s.distanceM;
                    m.useMiles = s.useMiles;
                    if (routes::active()) {
                        snprintf(m.routeLine, sizeof(m.routeLine),
                                 "%s · %.1f %s left", routes::activeName(),
                                 units::dist(routes::remainingKm(), s.useMiles),
                                 s.useMiles ? "mi" : "km");
                    } else {
                        snprintf(m.routeLine, sizeof(m.routeLine),
                                 "no route loaded");
                    }
                    ui_render_menu(m, fb);
                    break;
                }
                case SCREEN_SENSORS:
                case SCREEN_ROUTES:
                case SCREEN_HISTORY:
                    renderListScreen(fb);
                    break;
                case SCREEN_SETTINGS: {
                    SettingsInfo si{settings::ftpWatts(), settings::tzMinutes(),
                                    settings::backlight(), settings::useMiles()};
                    ui_render_settings(si, fb);
                    break;
                }
                case SCREEN_GPSDEBUG: {
                    GpsDebug d;
                    gps_service::getDebug(d);
                    GpsDebugView v;
                    v.moduleDetected = d.moduleDetected;
                    v.module = gps_service::moduleName();
                    v.chars = d.chars;
                    v.passedCksum = d.passedCksum;
                    v.failedCksum = d.failedCksum;
                    v.withFix = d.withFix;
                    v.satsInUse = d.satsInUse;
                    v.satsInView = d.satsInView;
                    v.bestSnr = d.bestSnr;
                    v.hdop = d.hdop;
                    v.locValid = d.locValid;
                    v.locAgeMs = d.locAgeMs;
                    v.lat = d.lat;
                    v.lon = d.lon;
                    v.altM = d.altM;
                    v.speedKmh = d.speedKmh;
                    v.hour = d.hour;
                    v.minute = d.minute;
                    v.second = d.second;
                    v.useMiles = settings::useMiles();
                    ui_render_gps_debug(v, fb);
                    break;
                }
            }
            if (navPrompt && !powerOverlay) {
                ui_render_nav_prompt(routes::activeName(),
                                     routes::maneuverCount(), fb);
            }
            if (powerOverlay) ui_render_power_sheet(s.recording, fb);
            // The nav banner is pure black/white, so DU is fine during nav.
            bool fast = screenIsFast(screen, powerOverlay) && !navPrompt;
            refresh(screenChanged, fast);
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

}  // namespace ui_dashboard
