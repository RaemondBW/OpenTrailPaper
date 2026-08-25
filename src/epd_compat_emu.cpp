// The EMULATOR panel half of epd_compat.h — see investigations/web-emulator.md.
//
// Instead of driving glass, epdc_paint() streams the 4bpp framebuffer out
// UART1 as an RLE frame, and the web page draws it on a canvas. The drawing
// half (all thirteen epdiy-signature functions) is shared with the other
// backends in epd_compat.cpp; only this file knows there is no panel.
//
// Wire protocol (matches web/emulator.js):
//   frame:  0xF5 'F' [u16 seq LE] [u32 rleLen LE] <rle bytes> 0xF6
//           rle = [count u8][value u8] pairs over the 4bpp buffer,
//                 540*960/2 = 259200 bytes expanded
//   clear:  0xF5 'C' 0xF6      (drive-to-white; canvas whitens, fb untouched)
//
// Input events arrive through a MAILBOX in DRAM, not the UART: QEMU's esp32s3
// model never delivers UART RX (tested on every port, both 2025/2026
// releases, with and without FIFO-filling padding), while TX is fine. So the
// bridge writes events straight into g_emuMailbox over QEMU's gdbstub
// (tools/emu/serve.py; -gdb tcp::3333 in run-qemu.sh) and pump() drains it.
// The event bytes (web/emulator.js sends them):
//   0xE1 [key u8][state u8]      key 1=BOOT 2=side; state 1=down 0=up
//   0xE2 [x u16 LE][y u16 LE]    touch down/move, portrait coords
//   0xE3                         touch up
//   0xE4                         home key press (one-shot)
//   0xE5 [len u8] <bytes>        NMEA for the GPS — looped into UART2's RX
//                                (QEMU's esp32s3 machine wires no third
//                                serial, so the GPS rides this wire too)
//   0xE6 [hr u8][pwr u16 LE][cad u8]   spoofed sensors, 0xFF/0xFFFF = absent —
//                                written into RideState with the same
//                                semantics the BLE notify handlers use
//                                (NimBLE itself is compiled out under QEMU)
//   0xE7 [pct u8][charging u8]   spoofed battery (the fuel gauge is absent)

#if defined(ARDUINO) && defined(OTP_EMULATOR)

#include "epd_compat.h"
#include "emu_input.h"

#include <Arduino.h>
#include <driver/uart.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "diag.h"
#include "ride_state.h"
#include "workout_service.h"
#include "workout.h"
#include "ride_recorder.h"

// A sample structured workout injected by the web "Load a workout" button
// (mailbox 0xE9). Plain-text ERG: minutes/watts pairs, the same format the
// phone app uploads. Sweet-spot 2×12 with a warmup ramp and a cooldown.
static const char kSampleWorkout[] =
    "[COURSE HEADER]\n"
    "MINUTES\tWATTS\n"
    "[END COURSE HEADER]\n"
    "[COURSE DATA]\n"
    "0\t100\n"
    "5\t170\n"     // 5 min warmup ramp 100 -> 170
    "5\t210\n"
    "17\t210\n"    // 12 min sweet spot
    "17\t130\n"
    "22\t130\n"    // 5 min recovery
    "22\t210\n"
    "34\t210\n"    // 12 min sweet spot
    "34\t120\n"
    "39\t120\n"    // 5 min cooldown
    "[END COURSE DATA]\n";

namespace {

constexpr int W = 540, H = 960;              // rotated, what the UI draws in
constexpr size_t FB_BYTES = 540UL * 960UL / 2;

uint8_t* fb = nullptr;
uint16_t frameSeq = 0;

void putU16(uint16_t v) { Serial1.write(v & 0xFF); Serial1.write(v >> 8); }
void putU32(uint32_t v) { putU16(v & 0xFFFF); putU16(v >> 16); }

}  // namespace

bool epdc_begin() {
    // UART1 is the frame/event wire. Pins are irrelevant under QEMU — the
    // peripheral index is what maps to the host's second -serial option — but
    // Serial1.begin still wants legal ones.
    Serial1.begin(921600, SERIAL_8N1, 17, 18);

    // PSRAM first (its home on hardware); internal RAM as the emulator
    // fallback when QEMU exposes no PSRAM. The UI task uses a static stack
    // under emulation (main.cpp) precisely so this 259 KB internal allocation
    // cannot starve it.
    fb = (uint8_t*)heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
    if (!fb) fb = (uint8_t*)heap_caps_malloc(FB_BYTES, MALLOC_CAP_INTERNAL);
    if (!fb) return false;
    memset(fb, 0xFF, FB_BYTES);   // white, matching a powered-on clean panel

    epd_set_rotation(EPD_ROT_PORTRAIT);
    diag::log("emu: panel backend up (%dx%d, frames on UART1)", W, H);
    return true;
}

uint8_t* epdc_framebuffer() { return fb; }

void epdc_paint() {
    if (!fb) return;
    // RLE in one pass straight to the UART: the driver task QEMU gives the
    // UART is effectively host-speed, and a dashboard frame is runs of white
    // almost everywhere, so this lands in tens of KB.
    Serial1.write(0xF5);
    Serial1.write('F');
    putU16(frameSeq++);

    // First pass to size the payload (the header carries rleLen so the web
    // side can frame the stream without lookahead).
    uint32_t rleLen = 0;
    for (size_t i = 0; i < FB_BYTES;) {
        uint8_t v = fb[i];
        size_t run = 1;
        while (run < 255 && i + run < FB_BYTES && fb[i + run] == v) run++;
        i += run;
        rleLen += 2;
    }
    putU32(rleLen);
    for (size_t i = 0; i < FB_BYTES;) {
        uint8_t v = fb[i];
        size_t run = 1;
        while (run < 255 && i + run < FB_BYTES && fb[i + run] == v) run++;
        Serial1.write((uint8_t)run);
        Serial1.write(v);
        i += run;
    }
    Serial1.write(0xF6);
}

// Tell the web page which VIEW the firmware is showing, so it can substitute a
// browser-rendered map for the map page (QEMU can't render maps — no PSRAM). One
// byte on the same UART wire, only when it changes: 0xF5 'P' <code> 0xF6, where
// code 2 == the map screen (DP_MAP), anything else == "a page you should draw
// from the firmware's own frames". See web/emulator/emulator.js + map_wasm.cpp.
static int g_viewCur = 0;    // the view most recently reported by the UI
static int g_viewLast = -1;  // the view the page has been told about

static void emitViewMarker(int view) {
    Serial1.write(0xF5);
    Serial1.write('P');
    Serial1.write((uint8_t)view);
    Serial1.write(0xF6);
}

void epdc_emit_view(int view) {
    g_viewCur = view;
    if (view == g_viewLast) return;
    g_viewLast = view;
    emitViewMarker(view);
}

// The map zoom + track-up state, so the browser-rendered map matches what the
// firmware's own on-screen zoom / north-up buttons did (the firmware handles the
// taps; the browser just renders at the state it reports). 0xF5 'M' <mpp> <up>.
static int g_mppCur = 2, g_upCur = 0;
static int g_mppLast = -1, g_upLast = -1;

static void emitMapMarker(int mpp, int up) {
    Serial1.write(0xF5);
    Serial1.write('M');
    Serial1.write((uint8_t)mpp);
    Serial1.write((uint8_t)up);
    Serial1.write(0xF6);
}

void epdc_emit_mapstate(int mpp, int trackUp) {
    g_mppCur = mpp;
    g_upCur = trackUp;
    if (mpp == g_mppLast && trackUp == g_upLast) return;
    g_mppLast = mpp;
    g_upLast = trackUp;
    emitMapMarker(mpp, trackUp);
}

// Re-announce the current view + map state — called from the 0xE8 repaint so a
// page that connects mid-session (when nothing changed, so nothing would emit)
// still learns whether it's on the map page and at what zoom.
void epdc_repaint_view() {
    emitViewMarker(g_viewCur);
    emitMapMarker(g_mppCur, g_upCur);
}

// Report the device's storage inventory to the web page's Filesystem tab, as
// lines prefixed "[FS]" on the console channel (the page routes those to the tab
// instead of the console). The emulator has no mounted SD (QEMU has no working
// PSRAM, so the framebuffer fills DRAM and FATFS can't fit) — so this reports
// the device's REAL logical storage: the flash-embedded map, the loaded workout,
// and the live ride, none of which needs the card.
void emitFsInventory() {
    Serial.printf("[FS] BEGIN\n");
    Serial.printf("[FS] mount /sd | %s\n",
                  ride_recorder::sdMounted()
                      ? "mounted"
                      : "not mounted \xE2\x80\x94 no PSRAM in QEMU (framebuffer "
                        "fills DRAM); device runs cardless");
    // The San Francisco base map is compiled into flash (board_build.embed_files
    // = data/sf.ebm) and rendered in the browser via WASM — it is on the device
    // without needing the card.
    Serial.printf("[FS] file /maps/sf.ebm | 590372 | flash-embedded base map\n");

    WorkoutView wv;
    workout_service::view(wv);
    if (wv.loaded && wv.wk) {
        Serial.printf("[FS] file /workouts/%s.erg | %d segments | %lus | "
                      "loaded in RAM\n",
                      wv.wk->name, wv.segCount, (unsigned long)wv.totalSec);
    } else {
        Serial.printf("[FS] note /workouts | empty \xE2\x80\x94 use \"Load a "
                      "workout\"\n");
    }
    if (ride_recorder::isRecording()) {
        RideState s = g_state.snapshot();
        Serial.printf("[FS] file /rides/current.fit | %.2f km, %lus | "
                      "live (not persisted without the card)\n",
                      s.distanceM / 1000.0, (unsigned long)s.elapsedS);
    } else {
        Serial.printf("[FS] note /rides | empty \xE2\x80\x94 tap BOOT to start "
                      "a ride\n");
    }
    Serial.printf("[FS] END\n");
}

// Streaming in epdc_paint() is synchronous, so the async-drive choreography
// the real panel needs is all no-ops here.
void epdc_paint_wait() {}
void epdc_power_off_soon() {}
void epdc_power_off_wait() {}

void epdc_clear(int passes) {
    (void)passes;   // one message regardless — a canvas whitens in one step
    Serial1.write(0xF5);
    Serial1.write('C');
    Serial1.write(0xF6);
}

void epdc_clear_dirty(int tolerance) {
    (void)tolerance;
    // The residue this scrubs is a physics artefact of real ink. A canvas has
    // no invisible-grey failure mode, so resyncing is free and unnecessary.
}

// Match the shipping EPD_Painter default so the UI makes the same
// screentone-vs-grey decisions the device makes.
int epdc_grey_levels() { return 4; }

// ---------------------------------------------------------------------------
// Input: the web page's events, decoded off the same wire.
// ---------------------------------------------------------------------------

namespace emu_input {

namespace {
bool bootState = false, sideState = false, homePending = false;
bool touchState = false;
int16_t touchX = 0, touchY = 0;

// A completed down->up tap, latched the moment the release is processed. The UI
// loop drains the whole mailbox in one pass, so a down and its up often arrive
// together — sampling touchState alone would then never see the transition and
// the tap would be lost (the workout / map on-screen buttons "not working").
// Latching here makes a tap atomic no matter how the events are batched.
bool tapPending = false;
int16_t tapX = 0, tapY = 0;

// A release is HELD BACK until the press has been visible long enough for the
// UI loop's two-consecutive-reads debounce to catch it, so the web page's
// quickest tap still lands. The loop now polls the button every ~16 ms under
// emulation (UI_IDLE_TICK_MS + a forced read each iteration), so two reads take
// ~32 ms — 60 ms gives a comfortable margin while keeping taps feeling instant.
// A deliberate hold (the 1.5 s power dialog) is unaffected.
constexpr uint32_t MIN_HOLD_MS = 60;
uint32_t bootDownAt = 0, sideDownAt = 0;
uint32_t bootReleaseAt = 0, sideReleaseAt = 0;   // 0 = no release pending

void setKey(bool& state, uint32_t& downAt, uint32_t& releaseAt, bool down) {
    if (down) {
        state = true;
        downAt = millis();
        releaseAt = 0;
    } else if (state) {
        const uint32_t held = millis() - downAt;
        releaseAt = millis() + (held >= MIN_HOLD_MS ? 0 : MIN_HOLD_MS - held);
        if (held >= MIN_HOLD_MS) state = false;
    }
}

bool keyLevel(bool& state, uint32_t& releaseAt) {
    if (state && releaseAt && (int32_t)(millis() - releaseAt) >= 0) {
        state = false;
        releaseAt = 0;
    }
    return state;
}

}  // namespace

// Payload length for each event opcode; -1 = variable (0xE5's own length byte).
int eventLen(uint8_t op) {
    switch (op) {
        case 0xE1: return 2;
        case 0xE2: return 4;
        case 0xE3: return 0;
        case 0xE4: return 0;
        case 0xE8: return 0;
        case 0xE9: return 0;
        case 0xEA: return 0;
        case 0xEB: return 0;
        case 0xEC: return 4;
        case 0xE5: return -1;
        case 0xE6: return 4;
        case 0xE7: return 2;
        default: return -2;   // unknown — drop the byte and resync
    }
}

void applyEvent(const uint8_t* p, int op) {
    switch (op) {
        case 0xE1:
            if (p[0] == 1) setKey(bootState, bootDownAt, bootReleaseAt, p[1] != 0);
            if (p[0] == 2) setKey(sideState, sideDownAt, sideReleaseAt, p[1] != 0);
            break;
        case 0xE2:
            touchX = (int16_t)(p[0] | (p[1] << 8));
            touchY = (int16_t)(p[2] | (p[3] << 8));
            touchState = true;
            break;
        case 0xE3:
            if (touchState) { tapPending = true; tapX = touchX; tapY = touchY; }
            touchState = false;
            break;
        case 0xEC:                        // atomic TAP: one reliable event, not
                                          // a down/up pair the loop must correlate
            tapPending = true;
            tapX = (int16_t)(p[0] | (p[1] << 8));
            tapY = (int16_t)(p[2] | (p[3] << 8));
            touchState = false;
            break;
        case 0xE4: homePending = true; break;
        case 0xE8:                        // repaint request (page just connected)
            epdc_repaint_view();          // re-announce the view (map vs other)
            epdc_paint();
            break;
        case 0xE9:                        // load + start the sample workout
            if (workout_service::loadText(kSampleWorkout, "SWEET SPOT 2x12"))
                workout_service::start();
            break;
        case 0xEA:                        // full reboot (page asked for a reset)
            Serial.println("[emu] reboot requested from the web page");
            Serial.flush();
            esp_restart();
            break;
        case 0xEB: emitFsInventory(); break;   // storage inventory for the FS tab
        case 0xE6:
            g_state.with([&](RideState& s) {
                const uint32_t now = millis();
                const uint8_t hr = p[0];
                const uint16_t pwr = p[1] | (p[2] << 8);
                const uint8_t cad = p[3];
                if (hr != 0xFF) {
                    s.heartRateBpm = hr;
                    s.hrMs = now;
                    s.hrConnected = true;
                } else {
                    s.hrConnected = false;
                }
                if (pwr != 0xFFFF) {
                    // No 3 s ring without the BLE stack; a 1 Hz spoof is its
                    // own average.
                    s.powerW = pwr;
                    s.power3sW = pwr;
                    s.powerMs = now;
                    s.powerConnected = true;
                } else {
                    s.powerConnected = false;
                }
                if (cad != 0xFF) {
                    s.cadenceRpm = cad;
                    s.cadenceMs = now;
                    s.cadenceConnected = true;
                } else {
                    s.cadenceConnected = false;
                }
            });
            break;
        case 0xE7:
            g_state.with([&](RideState& s) {
                s.batteryPercent = p[0];
                s.charging = p[1] != 0;
            });
            break;
    }
}

// NEVER blocks: bytes accumulate in this buffer across pump() calls and an
// event only applies once it is whole. The first version busy-waited for an
// event's tail inside pump() — one packet split across a TCP boundary froze
// the whole UI task mid-spin.
uint8_t evBuf[600];
int evLen = 0;

void pumpBuffer() {
    int used = 0;
    while (used < evLen) {
        const uint8_t op = evBuf[used];
        const int need = eventLen(op);
        if (need == -2) { used++; continue; }              // resync
        if (need == -1) {                                  // 0xE5 [len] <bytes>
            if (used + 2 > evLen) break;
            const int n = evBuf[used + 1];
            if (used + 2 + n > evLen) break;
            for (int i = 0; i < n; i++) gpsFeed(evBuf[used + 2 + i]);
            used += 2 + n;
            continue;
        }
        if (used + 1 + need > evLen) break;
        applyEvent(&evBuf[used + 1], op);
        used += 1 + need;
    }
    if (used > 0) {
        memmove(evBuf, evBuf + used, evLen - used);
        evLen -= used;
    }
}

void pump() {
    // Host-written ring: the bridge writes bytes at [head], then advances
    // head; we own tail. Internal DRAM is uncached, so no barrier dance.
    while (g_emuMailbox.tail != g_emuMailbox.head && evLen < (int)sizeof(evBuf)) {
        evBuf[evLen++] = g_emuMailbox.buf[g_emuMailbox.tail & (EMU_MAILBOX_SIZE - 1)];
        g_emuMailbox.tail++;
    }
    pumpBuffer();
}

bool bootDown() { return keyLevel(bootState, bootReleaseAt); }
bool sideDown() { return keyLevel(sideState, sideReleaseAt); }

bool takeHomePress() {
    const bool p = homePending;
    homePending = false;
    return p;
}

bool takeTap(int16_t* x, int16_t* y) {
    if (!tapPending) return false;
    tapPending = false;
    *x = tapX;
    *y = tapY;
    return true;
}

bool touchDown(int16_t* x, int16_t* y) {
    *x = touchX;
    *y = touchY;
    return touchState;
}

void gpsFeed(uint8_t b) { EmuSerialGPS.feed(b); }

}  // namespace emu_input

EmuGpsSerial EmuSerialGPS;
EmuMailbox g_emuMailbox = {0x4F54504D /* 'OTPM' */, 0, 0, {0}};

#endif  // ARDUINO && OTP_EMULATOR
