#include "ui_dashboard.h"

#include <Arduino.h>
#include <SD.h>
#include <Update.h>
#include <Wire.h>
#include "epd_compat.h"
#include <TouchDrvGT911.hpp>

#include "config.h"
#include "ride_state.h"
#include "ride_recorder.h"
#include "sd_bus.h"
#include "gps_service.h"
#include "board_power.h"
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp32-hal-tinyusb.h>   // usb_persist_restart / RESTART_BOOTLOADER
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include "ui_render.h"
#include "map_view.h"
#include "map_tiles.h"
#include "map_store.h"
#include "i2c_bus.h"
#include "usb_storage.h"
#include "ble_sensors.h"
#include "ble_server.h"
#include "routes.h"
#include "settings.h"
#include "diag.h"
#include "smooth_epd.h"
#include "power_mgmt.h"

namespace {

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
              SCREEN_SETTINGS, SCREEN_GPSDEBUG, SCREEN_DIRECTIONS };
Screen screen = SCREEN_DASH;
RideSummary pendingSummary;

float mapMpp = 2.0f;  // map zoom: 1/2/4/8/16/32 m per px (wide levels show tiles)
bool mapTrackUp = false;

// Serial test hooks: drive the UI over the CDC serial port to profile the map
// without physical taps. Toggle timing logs with 't'; single-char commands
// injected in the task loop mirror button presses. See pollSerialCommands().
bool dbgTiming = false;

// When the "Start navigation?" prompt appeared (for the accept settle guard).
uint32_t navPromptShownAt = 0;

// Turn-by-turn banner rect (top of map/dashboard); tapping it ends nav.
const EpdRect kNavBanner = {0, ui::STATUS_H, 540, 138};
void drawNavBanner(uint8_t* fb);
void buildMapScreenData(const RideState& s, MapScreenData& map);

// Which screen the Sensors page was opened from, so back returns there.
Screen sensorsFrom = SCREEN_MENU;

// Same for the upcoming-directions list, reached by tapping the turn banner.
Screen directionsFrom = SCREEN_MAP;

// Set from the GT911 home-button callback (fires inside touch.getPoint()).
volatile bool homeKeyPressed = false;

// Interrupt flags. ISRs only set these; all I2C reads and logic stay in the
// UI task. A slow fallback poll (see the task loop) covers the case where an
// INT line doesn't behave as expected, so input can never be dropped.
volatile bool touchIrq = false;      // GT911 INT (GPIO3)
volatile bool boardBtnIrq = false;   // BOOT edge or expander INT (GPIO38)

// Wakes the UI task from its idle block. Everything the loop services is either
// interrupt-driven (touch, buttons) or on its own slower timer (redraw 1 Hz,
// touch-poll fallback 200 ms, elevation 2.5 s), so the task has no reason to
// spin — it blocks here and the ISRs below release it.
//
// MUST be a dedicated semaphore, NEVER the task's built-in notification slot.
// ESP-IDF drivers use the CALLING task's notification slot to wait for
// completion — including the SPI master behind the SD card and the panel's DMA
// waits — and this task does both SD reads and panel painting. An input
// interrupt landing mid-transfer would corrupt a driver's completion handshake.
SemaphoreHandle_t uiWake = nullptr;

// Idle block length. 200 ms is the loop's own touch-poll backstop, so nothing
// inside it is starved; input latency is unaffected because the ISRs release
// the semaphore immediately.
constexpr uint32_t UI_IDLE_TICK_MS = 200;

inline void IRAM_ATTR uiWakeFromIsr() {
    if (!uiWake) return;
    BaseType_t hpw = pdFALSE;
    xSemaphoreGiveFromISR(uiWake, &hpw);
    if (hpw) portYIELD_FROM_ISR();
}

void IRAM_ATTR onTouchIrq() { touchIrq = true; uiWakeFromIsr(); }
void IRAM_ATTR onBoardBtnIrq() { boardBtnIrq = true; uiWakeFromIsr(); }

// Power/shutdown dialog overlay (opened by holding BOOT 1.5 s).
bool powerOverlay = false;

// Backlight: 4 levels cycled by the GPIO48 button.
// Off / Low / Med / Bright. Low is deliberately very dim — it is for reading the
// panel at night, where 50/255 was still dazzling; e-paper needs far less
// frontlight than an emissive display to become legible.
const uint8_t kBacklightPWM[4] = {0, 12, 90, 230};
void applyBacklight(int level) {
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    analogWrite(BOARD_BL_EN, kBacklightPWM[level]);
}

void shutdownDevice(uint8_t* fb, const char* reason) {
    // Hold light sleep off for the whole shutdown, and never release it.
    //
    // LOAD-BEARING, not just a bus guard. Automatic light sleep re-arms the RTC
    // timer wake source on EVERY tickless idle — esp_pm's vApplicationSleep
    // calls esp_sleep_enable_timer_wakeup() before each nap — and that trigger
    // lives in the same global sleep config esp_deep_sleep_start() later
    // consumes. If an idle window lands between our disable-everything below
    // and the deep-sleep call, the timer trigger comes back and deep sleep
    // honours it: the device wakes seconds later and boots, "reset: wake from
    // sleep". Taking the lock first means no idle can arm anything from here on.
    power_mgmt::busyAcquire();   // deliberately never released — we don't return

    // Log WHY we're powering off and flush it to SD before deep sleep, so the
    // next boot's log distinguishes a user shutdown / auto-sleep from a reset
    // or a power loss (which leave no such line).
    diag::log("shutdown: %s", reason);
    if (ride_recorder::isRecording()) {
        ride_recorder::stopRide(true);  // never lose a ride to power-off
    }
    diag::flushToSD();

    // Leave a static farewell on the glass — e-paper keeps it for free.
    // Full-screen map backdrop (last known position) with a POWERED OFF plate.
    // Force the widest zoom so the farewell shows the broadest area overview
    // (we're deep-sleeping right after, so mutating mapMpp is harmless).
    memset(fb, 0xFF, fbSize);
    {
        mapMpp = 32.0f;   // max zoom-out (1/2/4/8/16/32 m per px)
        // Always north-up for the farewell. This image sits on the glass for
        // days; rotated to whatever heading the last fix happened to have, it is
        // unreadable as a map of where the bike is. (Deep sleep follows
        // immediately, so mutating the live setting here is harmless.)
        mapTrackUp = false;
        RideState s = g_state.snapshot();
        // Save the last known position on the way down, so the next boot's
        // GPS warm-start seed is as fresh as possible.
        if (s.everHadFix) settings::setLastPosition(s.latitude, s.longitude);
        MapScreenData map = {};
        buildMapScreenData(s, map);
        ui_render_map_features(map, s, fb);
    }
    ui_render_shutdown_screen(fb);
    if (shadowFb) memcpy(shadowFb, fb, fbSize);
    // The shutdown screen has to survive on the glass for days with no power, so
    // it gets the one place a HARD clear is genuinely worth its cost: scrub to
    // white, then paint the final image with everything driven from a known
    // state. epdc_clear() leaves our framebuffer alone (unlike epdiy's
    // fullclear, which wiped it and needed a save/restore around the call).
    epdc_clear();
    epdc_paint();
    // MUST come before deep sleep. On the EPD_Painter backend epdc_paint() only
    // hands the frame to the driver's paint task and returns while the rows are
    // still being clocked out, so powering down here caught the panel mid-drive
    // and left the farewell screen half-written. (The epdiy backend painted
    // inline, which is why this never used to be needed.)
    epdc_paint_wait();

    // Close the SD cleanly before the card loses its host. An interrupted SD
    // transaction leaves the card's controller refusing CMD0 on the next boot —
    // cardType=NONE, unrecoverable by retrying, and it survives power cycles
    // until the card is reformatted. This cannot help an unexpected reset, but
    // the planned paths (power-off dialog, auto-sleep) have no excuse to leave
    // the card mid-transaction.
    sdLock();
    SD.end();
    sdUnlock();

    // Peripherals down, matching the factory sleep sequence
    i2cLock(); touch.sleep(); i2cUnlock();
    digitalWrite(BOARD_TOUCH_RST, LOW);
    gpio_hold_en((gpio_num_t)BOARD_TOUCH_RST);
    gpio_deep_sleep_hold_en();
    board_radio_power(false);

    // Wait for BOOT to come back up before arming the wake on it. The power
    // dialog is opened by HOLDING BOOT, so the button can still be down when the
    // rider taps Shut down — and ext0 waits for LOW, which a held button already
    // satisfies. Bounded, so a stuck button can't hang the shutdown.
    for (uint32_t t0 = millis();
         digitalRead(BOARD_BOOT_BTN) == LOW && millis() - t0 < 3000; ) {
        delay(20);
    }

    // ext0 wakes on GPIO0 going LOW, so the pin MUST be held high while asleep.
    // pinMode(INPUT_PULLUP) sets the *digital* pull-up, and that does not survive
    // the pad switching to RTC-domain control on the way into deep sleep — the
    // pin then floats, reads LOW, and the device wakes immediately. That is the
    // "pressing Shut down just restarts it" bug, and it hit auto-sleep too:
    // 2026-08-02 shows three auto-sleeps waking after exactly 1 s and a manual
    // power-off waking after 3 s, while one power-off that happened to settle
    // high stayed asleep 4 h 16 m. Intermittent because a floating pin is.
    rtc_gpio_pullup_en((gpio_num_t)BOARD_BOOT_BTN);
    rtc_gpio_pulldown_dis((gpio_num_t)BOARD_BOOT_BTN);

    // Start from NO wake sources. The enabled-trigger set is global and sticky:
    // anything enabled earlier in the boot — the PM tickless-idle timer above
    // all, but also the optional GPS UART wakeup — is still armed here, and
    // esp_deep_sleep_start() honours every one of it. That is why power-off kept
    // "restarting by itself after ~10 s": the last light sleep's RTC alarm was
    // simply re-armed for deep sleep. The button is the ONLY way back on.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
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
// Set by any input so the next loop iteration redraws immediately instead of
// waiting for the 1 Hz periodic tick — otherwise an in-place change (zoom, a
// map toggle) can sit up to a second before the panel repaints.
bool forceDraw = false;
// Last touch/button. Used to back off the barometer sample while the rider is
// interacting, so it does not contend with the recorder for the SD lock.
uint32_t lastUiInputMs = 0;
void noteActivity() {
    lastActivityMs = millis();
    lastUiInputMs = millis();
    forceDraw = true;
    if (uiWake) xSemaphoreGive(uiWake);   // redraw now, don't wait out the block
    ble_sensors::noteActivity();
}

// Panel refresh.
//
// This used to be ~260 lines of waveform policy: DU vs GL16 vs GC16, ghostDebt
// accounting, a deferred settle-clean, region-scoped two-phase flashes through
// white, a changed-region differ, and a pass that flattened the framebuffer to
// 1-bit. ALL of it existed to work around epdiy's differential DU — a pixel that
// is black before and after gets no waveform, DU is not DC-balanced, and neither
// DU nor GL16 can restore a white pixel it never drove. See
// investigations/display-ghosting.md for the full history.
//
// EPD_Painter makes every one of those workarounds unnecessary: it has its own
// delta engine, per-panel measured waveform tables, and an explicit DC-balance
// pass. Soaked on hardware with a 1 Hz partial update, the white background held
// with no drift — which is what none of our epdiy schemes achieved.
//
// So the whole policy reduces to: skip identical frames, otherwise paint.
// How much clean area one dirty rectangle may swallow before it is split, in
// pixels. Deliberately generous: the driver stops scanning at 32 rectangles and
// leaves everything past that unscrubbed, so a tolerance low enough to produce
// tight rects would silently skip the bottom of a busy transition — the exact
// residue this is here to remove. ~24 clean rows of the panel's 960 px width,
// which merges a map's scattered street rows into a handful of bands while
// still leaving an unchanged half-screen alone.
constexpr int kDirtyClearTolerance = 24 * 960;

bool refresh(bool screenChanged, bool fastInPage, bool listFast,
             bool forceClean = false, bool gc16 = false, bool fullFlash = false) {
    // screenChanged/fastInPage/listFast/forceClean/gc16 each selected an epdiy
    // waveform. The driver makes that choice itself now, so they survive only to
    // keep the ~15 call sites unchanged.
    //
    // fullFlash is the exception and IS still live — this comment used to claim
    // it was inert while the code below acted on it, because the scrub was
    // removed and then put back when residue showed up on the map, and only the
    // code changed. See the epdc_clear_dirty() call for what it now does.
    (void)screenChanged; (void)fastInPage; (void)listFast;
    (void)forceClean; (void)gc16;

    uint8_t* fb = epdc_framebuffer();

    // Identical frame: never touch the panel. Cheaper than letting the delta
    // engine discover there is nothing to do, and it keeps the 1 Hz idle path
    // free of panel activity entirely.
    if (shadowFb && memcmp(fb, shadowFb, fbSize) == 0) return false;
    if (shadowFb) memcpy(shadowFb, fb, fbSize);

    const uint32_t tw0 = millis();
    // Scrub before painting the map. A map is nearly all fine dark lines on
    // white; the dashboard it replaces is large filled blocks and heavy type.
    // Driving straight from one to the other leaves the dashboard's residue
    // sitting under the streets, and on this panel that reads as grey haze
    // exactly where map detail needs contrast.
    //
    // The residue is NOT over-driven black — the driver's delta engine can't
    // drive a black pixel black again (see epdc_clear_dirty). It is black that
    // failed to fully erase in one light pulse, which the driver has already
    // recorded as white, so no later frame will ever touch it again. A clear is
    // what resyncs the driver's model to the glass.
    //
    // SCOPED, not the whole panel: only the rectangles that actually differ
    // between the glass and this frame. Paid only when ENTERING the map — not
    // on the way out, not when the power sheet opens over it, and not on the
    // 1 Hz map redraws.
    if (fullFlash) epdc_clear_dirty(kDirtyClearTolerance);
    epdc_paint();

    if (dbgTiming)
        diag::log("refresh: %lums", (unsigned long)(millis() - tw0));
    refreshCount++;
    return true;
}


// Screens that are pure black/white AND update in place can use the fast
// DU path between frames. Screens with gray tones must take GL16.
bool screenIsFast(Screen s, bool overlay) {
    if (overlay) return false;  // power sheet has gray subtitle
    return s == SCREEN_DASH || s == SCREEN_MAP || s == SCREEN_GPSDEBUG;
}

// The menu / list / settings screens are pure black/white, so both their page
// changes and any in-place updates can take the fast DU path instead of the
// slower GL16 — this is what makes "switching menu state" feel snappy.
bool screenListFast(Screen s) {
    return s == SCREEN_MENU || s == SCREEN_SENSORS || s == SCREEN_ROUTES ||
           s == SCREEN_HISTORY || s == SCREEN_SETTINGS ||
           s == SCREEN_DIRECTIONS;
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
        case SCREEN_DIRECTIONS: screen = directionsFrom; break;
        case SCREEN_SETTINGS:
        case SCREEN_ROUTES:
        case SCREEN_HISTORY:  screen = SCREEN_MENU; break;
        case SCREEN_SENSORS:  leaveList(); break;  // also stops the BLE scan
        case SCREEN_MENU:     screen = SCREEN_DASH; break;
        case SCREEN_MAP:      screen = SCREEN_DASH; break;
        // Back on the summary resumes the ride (non-destructive); SAVE / DISCARD
        // must be tapped explicitly.
        case SCREEN_SUMMARY:  screen = SCREEN_DASH; break;
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

// Side-key short-press: step the backlight Off -> Low -> Med -> Bright -> Off.
void cycleBacklight() {
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
            // LATER: don't just stop navigating — fully unload the route so it
            // isn't drawn on the map. It's still saved on the SD card, so it can
            // be started later from Saved Routes.
            routes::clearRoute();
        }
        return;
    }

    // Bottom BACK strip on every sub-screen.
    if ((screen == SCREEN_SETTINGS || screen == SCREEN_SENSORS ||
         screen == SCREEN_ROUTES || screen == SCREEN_HISTORY ||
         screen == SCREEN_GPSDEBUG || screen == SCREEN_DIRECTIONS) &&
        inRect(kBackBar, x, y)) {
        goBack();
        return;
    }

    // Tapping the turn banner opens the rest of the route's directions. It used
    // to end navigation, which is a destructive action on the one control a
    // rider is most likely to hit by accident — and it threw away the thing they
    // probably wanted, which is to see what is coming up. Navigation is ended
    // from the menu instead.
    if (routes::navActive() && inRect(kNavBanner, x, y) &&
        (screen == SCREEN_DASH || screen == SCREEN_MAP)) {
        directionsFrom = screen;
        screen = SCREEN_DIRECTIONS;
        noteActivity();
        return;
    }

    switch (screen) {
        case SCREEN_DASH:
        case SCREEN_MAP:
            if (screen == SCREEN_MAP) {
                int dx = x - kMapCompass.cx,
                    dy = y - mapCompassCy(routes::navActive());
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
                    if (mapMpp < 32.0f) mapMpp *= 2.0f;
                    break;
                }
            }
            // Status bar opens the menu. On the map, the DATA STRIP along the
            // bottom (below MAP_STRIP_TOP, clear of the zoom buttons at y 560
            // and 640) goes to the dashboard. The body of either screen does
            // nothing.
            //
            // Tapping anywhere used to flip, which made the map hostile to use:
            // every miss of the compass or a zoom button threw the rider off the
            // map, and on the dashboard a stray glove brush did the same. The
            // flip now has deliberate targets — that strip, and the Home key,
            // which is the only way back to the map.
            if (y < ui::STATUS_H) screen = SCREEN_MENU;
            else if (screen == SCREEN_MAP && y >= ui::MAP_STRIP_TOP)
                screen = SCREEN_DASH;
            break;
        case SCREEN_SUMMARY:
            if (inRect(kResumeButton, x, y)) {
                // Ride is still recording in the background — just go back to the
                // dashboard and keep going.
                screen = SCREEN_DASH;
            } else if (inRect(kSaveButton, x, y)) {
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
            bool inRows = y >= kMenuRowTop && row >= 0 && row < 5;
            // BACKLIGHT is NOT a toggle row: ui_render_settings draws it with the
            // -/+ stepper (four levels don't fit a switch), so it must be hit-
            // tested like one. It used to be listed here, which meant only the
            // toggle band — overlapping the + button — did anything: "-" fell
            // through to the "tap anywhere else" branch and bounced the rider
            // out to the menu instead of dimming the light.
            bool toggleRow = row == kSettingsUnitsRow || row == kSettingsUsbRow;
            bool minus = x >= kSettingsMinusX && x < kSettingsMinusX + kSettingsBtn;
            bool plus = x >= kSettingsPlusX && x < kSettingsPlusX + kSettingsBtn;
            bool toggleHit = x >= kSettingsToggleX &&
                             x < kSettingsToggleX + kSettingsToggleW;
            bool edited = false;
            if (inRows && !toggleRow && (minus || plus)) {
                int dir = plus ? 1 : -1;
                if (row == 0) settings::setFtpWatts(settings::ftpWatts() + dir * 5);
                if (row == 1) settings::setTzMinutes(settings::tzMinutes() + dir * 30);
                if (row == kSettingsBacklightRow) {
                    // Clamped, not wrapped: with a visible -/+ pair, "+" at
                    // Bright wrapping round to Off would read as a fault. (The
                    // side key still cycles — that one has nowhere else to go.)
                    int bl = settings::backlight() + dir;
                    if (bl < 0) bl = 0;
                    if (bl > 3) bl = 3;
                    settings::setBacklight(bl);
                    applyBacklight(bl);
                }
                edited = true;
            } else if (inRows && toggleRow && toggleHit) {
                if (row == kSettingsUnitsRow) {
                    settings::setUseMiles(!settings::useMiles());
                } else {   // USB drive
                    bool on = !settings::usbDrive();
                    settings::setUsbDrive(on);
                    usb_storage::setDriveEnabled(on);
                }
                edited = true;
            }
            if (edited) {
                g_state.with([](RideState& st) {
                    st.ftpW = (uint16_t)settings::ftpWatts();
                    st.tzMin = (int16_t)settings::tzMinutes();
                    st.useMiles = settings::useMiles();
                    st.clock24h = settings::clock24h();
                });
                ble_server::pushSettingsToPhone();   // mirror the edit to the app
            } else if (y >= kMenuRowTop && row == kSettingsGpsRow) {
                screen = SCREEN_GPSDEBUG;
            }
            // Anything else on this screen does NOTHING. Settings is the one
            // place where a stray touch used to navigate away mid-edit — reach
            // for a stepper, miss it by a few pixels, and you were back on the
            // menu with the change unmade. The capacitive Home key (goBack())
            // is the only way out.
            break;
        }
        case SCREEN_GPSDEBUG:
            // Experimental: a tap in the top band runs the smooth-drive
            // pipeline self-test; anywhere else navigates back to Settings.
            if (y < 150) {
                smooth_epd::selfTest();
                // The self-test drove the panel directly, behind the driver's
                // back, so its record of what is on glass is now wrong. A clear
                // puts both back in agreement at white; zeroing shadowFb (a
                // value the renderer never produces) forces a full redraw.
                epdc_clear();
                if (shadowFb) memset(shadowFb, 0, fbSize);
            } else {
                screen = SCREEN_SETTINGS;
            }
            break;
    }
}

void handlePowerTap(int x, int y) {
    if (inRect(kPowerShutdown, x, y)) {
        uint8_t* fb = epdc_framebuffer();
        shutdownDevice(fb, "user power-off (dialog)");  // does not return
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
    // Tell the map renderer to drop the compass below the turn banner when one
    // is drawn over the top of the viewport.
    map.navBannerVisible = routes::navActive();
    map.metersPerPixel = mapMpp;

    // Position priority: the device's own current GPS fix; else the connected
    // phone's recent location (fallback when our receiver is cold); else the
    // last known device position; else the persisted/default. Never jump to the
    // default mid-ride.
    double lat = DEFAULT_MAP_LAT, lon = DEFAULT_MAP_LON;
    bool phoneRecent = s.phoneFixValid && (millis() - s.phoneFixMs) < 15000;
    map.phonePosition = phoneRecent && !s.gpsFix;
    if (s.gpsFix) {
        lat = s.latitude;
        lon = s.longitude;
    } else if (phoneRecent) {
        lat = s.phoneLat;
        lon = s.phoneLon;
    } else if (s.everHadFix) {
        lat = s.latitude;
        lon = s.longitude;
    } else {
        settings::lastPosition(lat, lon);
    }
    map_store::renderInto(lat, lon, mapMpp, map.riderX, map.riderY, rot, map);
    map.hasMap = map_store::coversPosition(lat, lon);

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
    uint32_t m0 = millis();
    buildMapScreenData(s, map);
    uint32_t m1 = millis();
    ui_render_map(map, s, fb);
    if (dbgTiming)
        diag::log("map: project=%lu draw=%lu polys=%d (tiles=%d base=%d) "
                  "cls[maj=%d pri=%d sec=%d ter=%d min=%d path=%d] ntiles=%d mpp=%d",
                  (unsigned long)(m1 - m0), (unsigned long)(millis() - m1),
                  map.featureCount, map.tilePolys, map.featureCount - map.tilePolys,
                  map.clsCount[0], map.clsCount[1], map.clsCount[2], map.clsCount[3],
                  map.clsCount[4], map.clsCount[5],
                  map.projectedTiles, (int)mapMpp);
    drawNavBanner(fb);
}

// Turn-by-turn banner across the top, shown on the map AND the dashboard while
// navigating. Tapping it (kNavBanner) opens the directions list.
//
// Units come from the shared state, the same source the directions list reads,
// rather than settings:: directly. Both stay in sync today (the settings screen
// and the phone's BLE settings write both), but reading one fact from two
// places is what made the list and the banner disagree in the first place.
void drawNavBanner(uint8_t* fb) {
    if (!routes::navActive()) return;
    char instr[routes::MANEUVER_TEXT];
    float dist = 0;
    if (routes::nextTurn(instr, sizeof(instr), dist)) {
        ui_render_nav_banner(instr, dist, g_state.snapshot().useMiles, fb);
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

// Compact kind label for the Sensors row subtitle (must stay short so the row
// fits the screen width).
const char* shortKinds(uint8_t mask) {
    switch (mask & 0x7) {
        case 1: return "HR";
        case 2: return "Power";
        case 3: return "HR+Power";
        case 4: return "Cadence";
        case 5: return "HR+Cadence";
        case 6: return "Power+Cad";
        case 7: return "HR+Power+Cad";
        default: return "Sensor";
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
                // Status FIRST (short kind label second) so the important word
                // — connected/saved — is never clipped off the right edge.
                const char* kinds = shortKinds(c.kindsMask);
                if (c.connected) {
                    snprintf(rows[i].subtitle, sizeof(rows[i].subtitle),
                             "Connected · %s", kinds);
                } else if (c.paired) {
                    snprintf(rows[i].subtitle, sizeof(rows[i].subtitle),
                             "%s · %s", c.rssi ? "In range" : "Saved", kinds);
                } else {
                    snprintf(rows[i].subtitle, sizeof(rows[i].subtitle),
                             "%s · %d dBm", kinds, c.rssi);
                }
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
        case SCREEN_DIRECTIONS: {
            title = "DIRECTIONS";
            footer = routes::navActive() ? "distances update as you ride"
                                         : "navigation has ended";
            int n = routes::upcomingCount();
            for (int i = 0; i < n && count < (int)(sizeof(rows) / sizeof(rows[0]));
                 ++i) {
                char instr[routes::MANEUVER_TEXT];
                float dm = 0;
                if (!routes::upcomingTurn(i, instr, sizeof(instr), dm)) break;
                snprintf(rows[count].title, sizeof(rows[0].title), "%s", instr);
                RideState s = g_state.snapshot();
                char dtxt[16];
                units::navDist(dtxt, sizeof(dtxt), dm, s.useMiles);
                snprintf(rows[count].subtitle, sizeof(rows[0].subtitle),
                         "in %s", dtxt);
                rows[count].inverted = (i == 0);   // the turn you are riding to
                count++;
            }
            if (count == 0) {
                snprintf(rows[count].title, sizeof(rows[0].title), "No turns ahead");
                snprintf(rows[count].subtitle, sizeof(rows[0].subtitle),
                         "you are on the last leg");
                count++;
            }
            break;
        }
        case SCREEN_HISTORY: {
            title = "RIDE HISTORY";
            footer = "rides upload from /rides on the SD card";

            // A ride in progress gets a pinned row at the top; its still-open,
            // unfinalized FIT file is then skipped in the scan below.
            const char* activeFile = ride_recorder::currentRideFile();
            if (activeFile[0]) {
                RideState s = g_state.snapshot();
                snprintf(rows[count].title, sizeof(rows[0].title),
                         "Recording in progress");
                uint32_t secs = s.elapsedS;
                snprintf(rows[count].subtitle, sizeof(rows[0].subtitle),
                         "%.1f %s · %lu:%02lu:%02lu",
                         units::distM(s.distanceM, s.useMiles),
                         units::distLabel(s.useMiles),
                         (unsigned long)(secs / 3600),
                         (unsigned long)(secs / 60 % 60),
                         (unsigned long)(secs % 60));
                rows[count].inverted = true;
                count++;
            }

            // The recorder writes FIT records on this same SPI bus 1 Hz while
            // riding; scanning the directory without the lock corrupts that
            // traffic and returns a garbled/empty list (why history looked
            // inaccessible during a ride). Hold the lock for the whole scan.
            sdLock();
            File dir = SD.open(RIDE_DIR);
            if (dir) {
                for (File f = dir.openNextFile(); f && count < kMenuRowCount;
                     f = dir.openNextFile()) {
                    const char* base = strrchr(f.name(), '/');
                    base = base ? base + 1 : f.name();
                    // Skip subdirs, the in-progress file (shown above), and
                    // stub rides too small to be worth opening.
                    if (!f.isDirectory() && strcmp(base, activeFile) != 0 &&
                        f.size() >= RIDE_MIN_USEFUL_BYTES) {
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
            sdUnlock();
            break;
        }
        default:
            break;
    }
    ui_render_list(title, rows, count, footer, fb);
}

}  // namespace

// Boot progress trace. ui_dashboard::begin() drives the panel through a lot of
// steps that only ever ran once epdc_begin() started succeeding, and a hang in
// any of them stops setup() before the tasks are created — which looks, from the
// outside, exactly like a dead device: blank screen and total serial silence.
// The last line printed says where it stopped.
//
// Gated with the rest of the bring-up aids: this is diagnostic scaffolding, and
// the shipping t5s3-pro build (the one CI releases) should not carry it.
#ifdef EPDC_BOOT_WAIT
#define EPDC_STEP(msg) Serial.printf("[ui] %lu ms: %s\n", (unsigned long)millis(), msg)
#else
#define EPDC_STEP(msg) ((void)0)
#endif

namespace ui_dashboard {

// Boot progress state. Kept tiny and static — this runs before any allocation
// we control, and a boot screen that can fail to allocate is worse than none.
namespace {
const char* bootLines[6];
int8_t      bootState[6];
int         bootCount = 0;
bool        bootScreenLive = false;
}

// Panel-only bring-up, split out of begin() so setup() can show progress while
// the slow subsystems (SD, GPS, BLE) start. Everything here is display/input;
// nothing touches the SD card or the radios.
bool beginPanel() {
    // Panel up. Board config, VCOM, waveform tables and rotation all live inside
    // the compat layer now (see epd_compat.cpp) — including the NVS-stored
    // per-board tuning EPD_Painter loads at the end of its own begin().
    if (!epdc_begin()) {
        Serial.println("[ui] display init FAILED");
        return false;
    }

    EPDC_STEP("panel up");

    // Backlight — level set in Settings, applied here at boot.
    pinMode(BOARD_BL_EN, OUTPUT);
    applyBacklight(settings::backlight());

    // Deep-sleep shutdown latches the touch RST pin LOW (gpio_hold) to keep
    // the GT911 in reset. Release that hold on boot, or after a wake the
    // controller stays reset and touch never works (can't reach map/settings).
    gpio_hold_dis((gpio_num_t)BOARD_TOUCH_RST);
    gpio_deep_sleep_hold_dis();

    EPDC_STEP("backlight");

    touch.setPins(BOARD_TOUCH_RST, BOARD_TOUCH_INT);
    touchOk = touch.begin(Wire, GT911_SLAVE_ADDRESS_L, BOARD_SDA, BOARD_SCL);
    if (!touchOk) Serial.println("[ui] GT911 touch not found");

    EPDC_STEP("touch");

    // Interrupt-drive the inputs. The GT911 pulses its INT on a touch event;
    // the XL9555 pulls its INT low when a button changes; BOOT is a plain
    // GPIO edge. ISRs just set a flag that the task acts on.
    // Must exist before any ISR can fire (they no-op on null, but the task
    // blocks on it immediately).
    uiWake = xSemaphoreCreateBinary();

    attachInterrupt(digitalPinToInterrupt(BOARD_TOUCH_INT), onTouchIrq, FALLING);
    attachInterrupt(digitalPinToInterrupt(BOARD_BOOT_BTN), onBoardBtnIrq, CHANGE);
    pinMode(BOARD_PCA9535_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BOARD_PCA9535_INT), onBoardBtnIrq,
                    FALLING);

    // The capacitive Home button below the display is a GT911 key, reported
    // via this callback during touch.getPoint(). It navigates back.
    touch.setHomeButtonCallback([](void*) { homeKeyPressed = true; }, nullptr);

    // Scrub before the first paint. E-paper holds its image with no power, so
    // what is physically on the glass right now is the shutdown screen from the
    // last ride (or another firmware's screen entirely) while the driver's record
    // says white. One pass did not shift it on hardware; four does.
    EPDC_STEP("about to clear x4");
    epdc_clear(4);
    EPDC_STEP("cleared");

    fbSize = epd_width() / 2 * epd_height();
    shadowFb = (uint8_t*)heap_caps_malloc(fbSize, MALLOC_CAP_SPIRAM);

    routeScreenPts = (int16_t*)heap_caps_malloc(
        MAX_ROUTE_SCREEN_PTS * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);

    EPDC_STEP("buffers");
    bootScreenLive = true;
    bootStatus("Display", true);
    return true;
}

namespace {
// Repaint the boot screen from the current step table. Safe before beginPanel()
// (no framebuffer yet) and after the dashboard takes over (bootScreenLive).
void bootRepaint() {
    if (!bootScreenLive) return;
    uint8_t* fb = epdc_framebuffer();
    if (!fb) return;
    ui_render_boot_screen(FIRMWARE_VERSION, bootLines, bootState, bootCount, fb);
    epdc_paint();
    // WAIT for the rows to finish clocking out. epdc_paint() is asynchronous on
    // the EPD_Painter backend — it hands the frame to the driver's paint task
    // and returns mid-drive — so leaving one in flight means the NEXT paint is
    // issued into a panel that is still being driven, and the driver wedges:
    // the glass keeps whatever was on it and never updates again.
    //
    // This bit when the "Maps" step moved the last boot repaint from main.cpp
    // (where ~3 s of SD scan plus the whole BLE init happened to follow it, so
    // the paint always landed first) to the end of ui_dashboard::begin(), a few
    // hundred ms before the UI task's first frame. The panel then froze on the
    // boot screen for the rest of the session while the firmware ran on
    // perfectly happily behind it — console answering, GPS logging, 140 s uptime.
    //
    // Same rule shutdownDevice() already follows for the farewell screen. It
    // belongs here rather than at the handover because EVERY boot repaint is a
    // full-screen paint, and any of them can be the last one before something
    // else drives the panel. Bounded at 6 s inside epdc_paint_wait(), so this
    // cannot turn a slow panel into a hung boot.
    epdc_paint_wait();
    if (shadowFb) memcpy(shadowFb, fb, fbSize);   // keep the delta engine honest
}

// Index of `step` in the table, or -1. Compared by pointer first since every
// caller passes the same string literal, with a strcmp fallback.
int bootFind(const char* step) {
    for (int i = 0; i < bootCount; ++i)
        if (bootLines[i] == step || strcmp(bootLines[i], step) == 0) return i;
    return -1;
}
}  // namespace

// Announce a step that is ABOUT to run, so the glass shows what the device is
// working on rather than sitting on the previous step until this one finishes.
// Matters most for the SD mount, which can take seconds and used to look like a
// freeze. Pair every bootStep() with a bootStatus() carrying the same literal.
void bootStep(const char* step) {
    if (!bootScreenLive || bootFind(step) >= 0) return;
    if (bootCount >= (int)(sizeof(bootLines) / sizeof(*bootLines))) return;
    bootLines[bootCount] = step;
    bootState[bootCount] = BOOT_PENDING;
    ++bootCount;
    bootRepaint();
}

// Resolve a step to a tick or a cross. Updates the pending line in place when
// bootStep() announced it, otherwise appends a completed one (so callers that
// never announce still work).
void bootStatus(const char* step, bool ok) {
    if (!bootScreenLive) return;
    int i = bootFind(step);
    if (i < 0) {
        if (bootCount >= (int)(sizeof(bootLines) / sizeof(*bootLines))) return;
        i = bootCount++;
        bootLines[i] = step;
    }
    bootState[i] = ok ? BOOT_OK : BOOT_FAIL;
    bootRepaint();
}

bool begin() {
    // Load the best downloaded map for our last-known spot, else the embedded
    // default. New maps arrive from the phone over BLE (see map_store).
    //
    // ANNOUNCED, like the SD mount and for the same reason: indexing the tiles
    // on the card is the longest thing left in setup() — ~4 s with 107 tiles,
    // and it grows with the card — and it used to run with the glass showing a
    // fully ticked list and an unmoving bar. Nothing said the device was still
    // working, so the last four seconds of every boot looked like a hang.
    bootStep("Maps");
    double mlat = DEFAULT_MAP_LAT, mlon = DEFAULT_MAP_LON;
    settings::lastPosition(mlat, mlon);
    map_store::begin(mlat, mlon);
    bootStatus("Maps", true);
    EPDC_STEP("map loaded — begin() done");
    // Hand the panel over to the dashboard: further bootStatus() calls no-op,
    // and the task's first refresh() paints the real UI over the boot screen.
    bootScreenLive = false;
    return true;
}

// Flash a firmware.bin dropped on the SD card, then reboot. Reads straight from
// SD into the flash writer — no PSRAM staging (unlike the BLE OTA). ESP32 A/B
// OTA partitions still protect the running image if the flash fails. To update:
// copy the new firmware.bin to the SD card root and power the device on.
void applySdUpdate() {
    if (!ride_recorder::sdMounted() || !SD.exists("/firmware.bin")) return;

    // Rename first so a bad/interrupted flash can never boot-loop re-trying it.
    SD.remove("/firmware.applied");
    SD.rename("/firmware.bin", "/firmware.applied");
    File f = SD.open("/firmware.applied", FILE_READ);
    if (!f) return;
    size_t size = f.size();
    if (size < 4096) { f.close(); return; }

    auto drawProgress = [&](int pct, bool first) {
        uint8_t* fb = epdc_framebuffer();
        memset(fb, 0xFF, fbSize);
        ui_render_update_overlay("Installing", pct, fb);
        refresh(first, !first, false);   // GL16 on first frame, fast DU for progress
    };
    drawProgress(0, true);
    diag::log("sd update: flashing %u bytes from SD", (unsigned)size);

    bool ok = false;
    if (Update.begin(size)) {
        uint8_t buf[4096];
        size_t written = 0;
        int lastPct = -10;
        while (written < size) {
            size_t n = f.read(buf, sizeof(buf));
            if (n == 0) break;
            if (Update.write(buf, n) != n) break;
            written += n;
            int pct = (int)(written * 100 / size);
            if (pct - lastPct >= 10) { lastPct = pct; drawProgress(pct, false); }
        }
        ok = (written == size) && Update.end(true);
    }
    f.close();
    if (ok) {
        SD.remove("/firmware.applied");   // done — don't keep it around
        diag::log("sd update OK — rebooting into new firmware");
        diag::flushToSD();
        drawProgress(100, false);
        epdc_paint_wait();   // same async-paint hazard as shutdownDevice(): the
                             // delay below is a guess, not a completion signal
        delay(600);
        esp_restart();
    } else {
        // Leave /firmware.applied (not /firmware.bin) so it isn't retried; the
        // device keeps running its current, untouched image.
        diag::log("sd update FAILED (err %d) — keeping current firmware",
                  Update.getError());
    }
}

// Reboot into the ROM serial bootloader (download mode) for hands-free
// reflashing. usb_persist_restart keeps the USB-CDC enumerated across the reset
// (same port, no re-enumeration) — unlike a raw FORCE_DOWNLOAD_BOOT + restart,
// which tears the USB down and leaves no port for the host to connect to.
static void rebootToBootloader() {
    Serial.println("[cmd] entering download mode (USB persists) — flash now");
    Serial.flush();
    delay(100);
    usb_persist_restart(RESTART_BOOTLOADER);
}

// After an `agnss <n>` command, the next n bytes on the console are raw AGNSS
// ephemeris (not console text) piped straight into the GPS inject stream.
static long agnssRxRemaining = 0;

static void printConsoleHelp() {
    Serial.println("commands:");
    Serial.println("  help                 this list");
    Serial.println("  cold [unaided]       GPS cold-start test (re-seed unless 'unaided')");
    Serial.println("  gpsoff <sec>         cut GPS power for N s, then re-seed (retention test)");
    Serial.println("  gpsver               query GPS module firmware version");
    Serial.println("  gpsraw <on|off>      echo raw receiver bytes");
    Serial.println("  sd                   SD mount state + cardType (NONE = card not answering)");
    Serial.println("  usbdrive [on|off]    expose the SD to a host; off takes the card back");
    Serial.println("  power                battery voltage + draw (mA) + full fuel-gauge state");
    Serial.println("  bootloader           reboot into download mode for flashing");
    Serial.println("  reboot               restart the device");
    Serial.println("  timing               toggle frame-timing logs");
    Serial.println("  screen <map|dash|menu|settings|gps>   switch UI screen");
    Serial.println("  zoom <in|out>        map zoom     back   go back");
}

// Dispatch one console line (first word = command, rest = args).
static void runConsoleLine(char* line) {
    char* cmd = strtok(line, " \t");
    if (!cmd) return;
    char* arg = strtok(nullptr, " \t");

    if (!strcasecmp(cmd, "help") || !strcmp(cmd, "?")) {
        printConsoleHelp();
    } else if (!strcasecmp(cmd, "cold")) {
        bool aided = !(arg && !strcasecmp(arg, "unaided"));
        Serial.printf("[cmd] GPS cold-start test (%s)\n", aided ? "aided" : "unaided");
        gps_service::forceColdStart(aided);
    } else if (!strcasecmp(cmd, "gpsoff")) {
        int sec = arg ? atoi(arg) : 5;
        if (sec < 1) sec = 1;
        Serial.printf("[cmd] GPS power off %ds, then re-seed\n", sec);
        gps_service::powerCycleTest(sec * 1000);
    } else if (!strcasecmp(cmd, "gpsver")) {
        Serial.println("[cmd] querying GPS version (watch for $GPTXT)");
        gps_service::queryVersion();
    } else if (!strcasecmp(cmd, "gpssend")) {
        if (!arg) { Serial.println("[cmd] gpssend <nmea-body, no $ or *cksum>"); return; }
        Serial.printf("[cmd] -> $%s\n", arg);
        gps_service::sendNmeaCommand(arg);
    } else if (!strcasecmp(cmd, "gpsraw")) {
        bool on = !(arg && !strcasecmp(arg, "off"));
        gps_service::setRawEcho(on);
        Serial.printf("[cmd] raw GPS echo %s\n", on ? "ON" : "OFF");
    } else if (!strcasecmp(cmd, "agnss")) {
        // Test hook: stream N raw AGNSS ephemeris bytes over serial into the
        // same pipe the BLE path uses. After this line, the next N bytes are
        // taken verbatim (see pollSerialCommands), not parsed as commands.
        long n = arg ? atol(arg) : 0;
        if (n <= 0) { Serial.println("[cmd] agnss <byte-count>, then send raw bytes"); return; }
        agnssRxRemaining = n;
        gps_service::agnssBegin();
        Serial.printf("[cmd] AGNSS: send %ld raw bytes now\n", n);
    } else if (!strcasecmp(cmd, "power")) {
        uint16_t mv = 0; int16_t ma = 0;
        if (board_read_power(mv, ma))
            Serial.printf("[power] %umV %dmA (%s)\n", mv, ma,
                          ma < 0 ? "discharging" : "charging/idle");
        else
            Serial.println("[power] fuel gauge init failed at boot "
                           "(raw registers still read below)");
        // Always dump the full gauge state: when the battery percentage is
        // missing, the status words and the raw SOC are the whole diagnosis.
        char rep[200];
        board_gauge_report(rep, sizeof(rep));
        Serial.printf("[gauge] %s\n", rep);
    } else if (!strcasecmp(cmd, "sd")) {
        // SD status without needing a boot log. The mount happens ~1.4 s into
        // boot, long before a USB-CDC host can attach, so for a long time the
        // only way to know why the card was missing was to win a race against
        // the console coming up. cardType is the useful bit: NONE means the card
        // is not answering at all (seating / wedged / dead), a real type with no
        // mount means the filesystem is the problem.
        uint8_t ct = SD.cardType();
        const char* cn = ct == CARD_NONE ? "NONE" : ct == CARD_MMC ? "MMC"
                       : ct == CARD_SD ? "SD" : ct == CARD_SDHC ? "SDHC" : "UNKNOWN";
        Serial.printf("[sd] mounted=%s cardType=%s size=%lluMB\n",
                      ride_recorder::sdMounted() ? "yes" : "NO", cn,
                      SD.cardSize() / (1024ULL * 1024ULL));
        // "Not mounted" has two causes that need opposite responses, and the
        // difference is invisible on the device's own screen. Say which it is.
        if (!ride_recorder::sdMounted()) {
            if (usb_storage::hostActive())
                Serial.println("[sd] a USB host owns the card — unplug it, eject it, "
                               "or toggle the USB drive off/on ('usbdrive off' then 'on')");
            else
                Serial.println("[sd] not mounted by the firmware — cardType=NONE means "
                               "the card is not answering (seating / wedged / dead)");
        }
        if (ride_recorder::sdMounted())
            Serial.printf("[sd] %llu MB free, log=%s\n",
                          (SD.totalBytes() - SD.usedBytes()) / (1024ULL * 1024ULL),
                          diag::logPath());
    } else if (!strcasecmp(cmd, "usbdrive")) {
        // The serial-side escape hatch for a stuck "a USB host owns the card":
        // turning the drive off hands the card straight back to the firmware.
        // Same switch as device Settings, so it persists.
        if (!arg) {
            Serial.printf("[usbdrive] %s (host %s the card)\n",
                          settings::usbDrive() ? "on" : "off",
                          usb_storage::hostActive() ? "HAS" : "does not have");
        } else {
            bool on = !strcasecmp(arg, "on") || !strcasecmp(arg, "1");
            settings::setUsbDrive(on);
            usb_storage::setDriveEnabled(on);
            ble_server::pushSettingsToPhone();   // keep the app's toggle in step
            Serial.printf("[usbdrive] turned %s\n", on ? "on" : "off");
        }
    } else if (!strcasecmp(cmd, "bootloader") || !strcasecmp(cmd, "boot")) {
        rebootToBootloader();
    } else if (!strcasecmp(cmd, "reboot")) {
        Serial.println("[cmd] rebooting");
        Serial.flush(); delay(80); esp_restart();
    } else if (!strcasecmp(cmd, "timing")) {
        dbgTiming = !dbgTiming;
        diag::log("dbg timing %s", dbgTiming ? "ON" : "OFF");
    } else if (!strcasecmp(cmd, "screen")) {
        if (!arg) { Serial.println("[cmd] screen <map|dash|menu|settings|gps>"); return; }
        if      (!strcasecmp(arg, "map"))      screen = SCREEN_MAP;
        else if (!strcasecmp(arg, "dash"))     screen = SCREEN_DASH;
        else if (!strcasecmp(arg, "menu"))     screen = SCREEN_MENU;
        else if (!strcasecmp(arg, "settings")) screen = SCREEN_SETTINGS;
        else if (!strcasecmp(arg, "gps"))      screen = SCREEN_GPSDEBUG;
        else { Serial.printf("[cmd] unknown screen '%s'\n", arg); return; }
        noteActivity();
    } else if (!strcasecmp(cmd, "zoom")) {
        if (arg && !strcasecmp(arg, "in") && mapMpp > 1.0f) mapMpp /= 2.0f;
        else if (arg && !strcasecmp(arg, "out") && mapMpp < 32.0f) mapMpp *= 2.0f;
        screen = SCREEN_MAP; noteActivity();
    } else if (!strcasecmp(cmd, "back")) {
        goBack(); noteActivity();
    } else {
        Serial.printf("[cmd] unknown '%s' (type 'help')\n", cmd);
    }
}

// Line-based serial console over the USB-CDC port. The one Serial reader in the
// firmware (a second reader would race it for bytes). Commands drive the UI for
// profiling and the flash/GPS iteration loop; see printConsoleHelp().
void pollSerialCommands() {
    static char buf[48];
    static uint8_t n = 0;
    while (Serial.available() > 0) {
        if (agnssRxRemaining > 0) {           // raw AGNSS byte-stream mode
            uint8_t b = (uint8_t)Serial.read();
            gps_service::agnssInject(&b, 1);
            if (--agnssRxRemaining == 0) {
                gps_service::agnssEnd();
                Serial.println("[cmd] AGNSS: all bytes received");
            }
            continue;
        }
        int c = Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            buf[n] = 0;
            if (n) runConsoleLine(buf);
            n = 0;
        } else if (n < sizeof(buf) - 1) {
            buf[n++] = (char)c;
        } else {
            n = 0;   // overflow — drop the line
        }
    }
}

void task(void*) {
    uint32_t lastDraw = 0;
    Screen lastScreen = screen;
    bool lastOverlay = false;
    bool lastNavPrompt = false;

    lastActivityMs = millis();
    int lastUpdatePct = -100, lastUpdatePhase = -1;

    // Ride recovery runs HERE, not in setup(). It is unbounded card work — it
    // once ran long enough to trip the interrupt watchdog, and since a reset
    // mid-recovery tears another file, every reboot found more to do than the
    // last. Off the boot path a slow recovery costs time, not a brick.
    ride_recorder::recoverRides();
    applySdUpdate();          // flash a firmware.bin from the SD card, if present
    usb_storage::begin();     // THEN expose the SD to a host computer over USB
    bool lastHostActive = false;

    for (;;) {
        // When the computer releases the SD (eject/unplug), check whether it
        // dropped a firmware.bin on the card and flash it automatically.
        bool host = usb_storage::hostActive();
        if (lastHostActive && !host) {
            applySdUpdate();
            map_store::rescanCard();   // it may have added/removed maps too
        }
        lastHostActive = host;

        // A card that only mounted after boot had given up: setup() skipped the
        // routes, the map index and the USB drive, and nothing revisited them, so
        // the card stayed mounted-but-unused for the whole session. Run that
        // bring-up now. It happens here, in the UI task, because the map cache is
        // this task's alone — doing it from loop() would race the renderer.
        if (ride_recorder::consumeLateMount()) {
            diag::log("sd: late mount — loading routes/maps, exposing USB drive");
            ride_recorder::recoverRides();
            routes::begin();
            map_store::rescanCard();
            applySdUpdate();      // a firmware.bin may have been on the card
            usb_storage::begin(); // no-op if it already came up
        }

        // Terrain elevation from the map DEM at the current position.
        // Only the UI task touches the tile cache, so this stays race-free with
        // the map render. The recorder reads mapElevationM to integrate ascent.
        //
        // This is an SD read, and it sits near the top of the loop — ahead of
        // touch handling. While a ride is recording, the recorder task (which
        // runs at a HIGHER priority than the UI) takes sdLock every second to
        // write a FIT record, and every 15 s to flush. A DEM read here then
        // blocks behind it and the rider's tap is not processed until it
        // returns, which is why touch felt sluggish only while recording.
        //
        // So skip it entirely while the rider is interacting, and sample at
        // 2.5 s rather than 1 s otherwise: ascent is integrated with 3 m
        // hysteresis, so a slower sample costs nothing but removes most of the
        // opportunities to collide with the recorder.
        static uint32_t lastElevMs = 0;
        const bool interacting = touchIrq || touchWasDown ||
                                 millis() - lastUiInputMs < 1500;
        if (!interacting && millis() - lastElevMs > 2500 && !host) {
            lastElevMs = millis();
            RideState es = g_state.snapshot();
            double elat = 0, elon = 0;
            bool have = false;
            if (es.gpsFix) { elat = es.latitude; elon = es.longitude; have = true; }
            else if (es.phoneFixValid && millis() - es.phoneFixMs < 15000) {
                elat = es.phoneLat; elon = es.phoneLon; have = true;
            } else if (es.everHadFix) { elat = es.latitude; elon = es.longitude; have = true; }
            if (have) {
                float ev = map_store::elevationAt(elat, elon);
                bool ok = (ev == ev);   // false when NAN
                g_state.with([&](RideState& st) {
                    st.mapElevationValid = ok;
                    if (ok) st.mapElevationM = ev;
                });
            }
        }

        // A firmware update in progress takes over the screen with a progress
        // modal (redrawn only when the percentage/phase moves so the e-paper
        // isn't thrashed). Everything else is paused until it finishes/reboots.
        if (ble_server::updateInProgress()) {
            int pct = ble_server::updatePercent();
            const char* phase = ble_server::updatePhase();
            int phaseId = phase[0] == 'I' ? 2 : 1;
            bool phaseChanged = phaseId != lastUpdatePhase;
            // Redraw the modal on phase change or every 10% (the progress bar
            // updates use the fast DU path, not a full GL16 — frequent
            // high-current full refreshes during the BLE transfer can disrupt
            // it). The current screen shows through behind the popup.
            if (phaseChanged || pct - lastUpdatePct >= 10 ||
                (pct == 100 && lastUpdatePct != 100)) {
                lastUpdatePct = pct;
                lastUpdatePhase = phaseId;
                RideState s = g_state.snapshot();
                s.phoneConnected = ble_server::isPhoneConnected();
                uint8_t* fb = epdc_framebuffer();
                memset(fb, 0xFF, fbSize);
                ui_render_dashboard(s, routes::navActive(), fb);   // backdrop
                ui_render_update_overlay(phase, pct, fb);          // modal on top
                refresh(phaseChanged, !phaseChanged, false);   // GL16 on phase change, else DU
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        lastUpdatePct = -100; lastUpdatePhase = -1;

        // Riding counts as activity even without touching the screen — a bike
        // computer must NEVER auto-off while you're moving. Any real speed keeps
        // it awake (and for the idle timeout after you stop).
        {
            RideState now = g_state.snapshot();
            if (now.gpsFix && now.speedKmh > 3.0f) lastActivityMs = millis();
        }

        // Auto-sleep after a quiet period to save battery — a bike computer
        // left on a desk otherwise burns power on GPS + BLE + CPU. Held off
        // while recording, navigating, moving, or a phone is connected. Wake
        // with BOOT.
        if (millis() - lastActivityMs > AUTO_SLEEP_MS &&
            !ride_recorder::isRecording() && !routes::navActive() &&
            !ble_server::isPhoneConnected()) {
            uint8_t* fb = epdc_framebuffer();
            shutdownDevice(fb, "auto-sleep (idle timeout)");   // does not return
        }

        // Apply a backlight level changed from the phone (BLE writes NVS but
        // not the PWM). Also covers the device's own +/- which already applied.
        static int lastBl = -1;
        if (settings::backlight() != lastBl) {
            lastBl = settings::backlight();
            applyBacklight(lastBl);
        }

        // Left-side physical buttons, each with its own debounce/hold state.
        // BOOT (GPIO0): short press starts/stops the ride, hold 1.5 s opens the
        // power dialog. Side key (expander PC12): short press cycles the
        // backlight. Interrupt-driven, with a 200 ms fallback poll; two
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
                    if (sideLow >= 2) { noteActivity(); cycleBacklight(); }
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
                i2cLock();
                bool down = touch.getPoint(x, y, 1) > 0;
                i2cUnlock();
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

        // Capacitive Home button (GT911 key). On the two main screens it swaps
        // dash <-> map — the gesture that used to be "tap anywhere", now on a
        // dedicated key so the map body is safe to touch. Everywhere else it
        // still navigates back. Debounced so one press triggers once even while
        // the key is held.
        if (homeKeyPressed) {
            homeKeyPressed = false;
            static uint32_t lastHome = 0;
            if (millis() - lastHome > 400) {
                lastHome = millis();
                noteActivity();
                if (screen == SCREEN_DASH)      screen = SCREEN_MAP;
                else if (screen == SCREEN_MAP)  screen = SCREEN_DASH;
                else                            goBack();
            }
        }

        pollSerialCommands();   // serial-driven button presses (testing/profiling)

        bool navPrompt = routes::navPending();
        if (navPrompt && !lastNavPrompt) navPromptShownAt = millis();
        bool screenChanged = screen != lastScreen || powerOverlay != lastOverlay
                             || navPrompt != lastNavPrompt;
        // Was: a deferred GL16 clean scheduled after an interactive DU burst
        // settled. The driver DC-balances its own output, so there is nothing
        // left to clean up after — and the ~950 ms hitch that clean cost is gone
        // with it. Kept as a constant only because refresh() still takes the
        // argument.
        const bool wantClean = false;
        bool navPromptAppearing = navPrompt && !lastNavPrompt;
        if (screenChanged || forceDraw || millis() - lastDraw >= 1000) {
            lastDraw = millis();
            forceDraw = false;
            Screen prevScreen = lastScreen;
            lastScreen = screen;
            lastOverlay = powerOverlay;
            lastNavPrompt = navPrompt;
            RideState s = g_state.snapshot();
            s.phoneConnected = ble_server::isPhoneConnected();   // for the status bar
            // Swap to a downloaded map that covers where we are, if one exists.
            // Not while a host computer owns the SD card.
            if (s.everHadFix && !usb_storage::hostActive())
                map_store::ensureForPosition(s.latitude, s.longitude);
            uint8_t* fb = epdc_framebuffer();
            memset(fb, 0xFF, epd_width() / 2 * epd_height());
            // While the "Start navigation?" prompt is up, the base screen shows
            // the whole route fitted so it can be recognized before accepting.
            if (navPrompt && !powerOverlay) {
              // Preview first (its road context projects full-screen), then the
              // status bar + accept sheet on top to mask any road spill.
              ui_render_route_preview(fb);
              ui::statusBar(s, fb);
              ui_render_nav_prompt(routes::activeName(),
                                   routes::maneuverCount(), fb);
            } else {
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
                case SCREEN_DIRECTIONS:
                    renderListScreen(fb);
                    break;
                case SCREEN_SETTINGS: {
                    SettingsInfo si{settings::ftpWatts(), settings::tzMinutes(),
                                    settings::backlight(), settings::useMiles(),
                                    settings::usbDrive()};
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
            if (powerOverlay) ui_render_power_sheet(s.recording, fb);
            }  // end else (normal screens)
            // These three classified the frame so refresh() could pick a waveform.
            // The driver picks its own now (see refresh()), so they are inert — kept
            // because they document which screens are pure black/white and which
            // transitions used to need scrubbing, and because deleting them would
            // mean touching all ~15 refresh() call sites for no behaviour change.
            bool fastInPage = screenIsFast(screen, powerOverlay) && !navPrompt;
            bool listFast = screenListFast(screen) && screenListFast(prevScreen) &&
                            !powerOverlay && !navPrompt;
            // ENTERING the map is the only transition that earns a scrub — see
            // refresh(). Leaving it doesn't: what replaces the map is heavy type
            // and filled blocks, which drive over fine map lines without any
            // haze, so that flash bought nothing and just made every exit from
            // the map feel slow. Deliberately NOT gated on screenChanged, which
            // is also true when only the power sheet opened or closed: that is
            // the popup flash, and staying on SCREEN_MAP now reads as no
            // transition at all.
            bool mapTransition = screen == SCREEN_MAP && prevScreen != SCREEN_MAP;
            refresh(screenChanged, fastInPage, listFast, wantClean,
                    navPromptAppearing, mapTransition);
        }

        // The epdiy path managed panel rails by hand here — poweron before an
        // update, and a 1.5 s keep-alive so successive taps skipped the ~36 ms
        // power-up. EPD_Painter owns its rails around each paint, so there is
        // nothing to release.

        // Block until an input interrupt or a redraw request, with a fallback
        // tick that still satisfies the slowest thing the loop owes anyone (the
        // 200 ms touch-poll backstop). Was a flat 30 ms delay, i.e. ~33
        // wakeups/s to discover there was nothing to do ~30 times out of 33.
        //
        // This only became worth fixing once light sleep worked: the SoC can
        // only sleep when BOTH cores are idle, and tickless idle sizes each
        // sleep by the shortest pending timer across them, so this task's tick
        // was capping every sleep on core 1 at 30 ms.
        xSemaphoreTake(uiWake, pdMS_TO_TICKS(UI_IDLE_TICK_MS));
    }
}

}  // namespace ui_dashboard
