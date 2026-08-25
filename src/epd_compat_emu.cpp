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

#include "diag.h"
#include "ride_state.h"

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

    // PSRAM first, like the real backends: the framebuffer is the exact
    // allocation the shipping firmware makes, and QEMU models the PSRAM size.
    fb = (uint8_t*)heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
    if (!fb) fb = (uint8_t*)malloc(FB_BYTES);
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

// A mouse click is ~50 ms; the button poll runs at 200 ms and there is no
// GPIO edge interrupt under emulation to vouch for a press between polls. So
// a release is HELD BACK until the press has been visible for one full poll
// period — the web page's quickest tap still lands, and a deliberate hold
// (the power dialog) is unaffected.
constexpr uint32_t MIN_HOLD_MS = 250;
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
        case 0xE3: touchState = false; break;
        case 0xE4: homePending = true; break;
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
