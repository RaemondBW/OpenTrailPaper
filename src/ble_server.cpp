#include "ble_server.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "nimble/nimble/host/services/gatt/include/services/gatt/ble_svc_gatt.h"
#include <SD.h>
#include <Update.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <atomic>

#include "config.h"
#include "ride_state.h"
#include "ride_recorder.h"
#include "settings.h"
#include "routes.h"
#include "gps_service.h"
#include "phone_motion.h"
#include "speed_source.h"
#include "ble_sensors.h"
#include "map_select.h"
#include "workout.h"
#include "workout_service.h"
#include "map_store.h"
#include "media.h"
#include "ams_client.h"
#include "ble_interval_policy.h"
#include "ble_pair_guard.h"
#include "sd_bus.h"
#include "tile_versions.h"
#include "usb_storage.h"
#include "dash_config.h"
#include "mesh_service.h"
#include "diag.h"
#include "ble_shared.h"
#include "link.h"

namespace {

// Custom 128-bit UUIDs — keep in sync with the iOS app.
const char* SVC_UUID      = "b1c50000-9e0f-4b7a-9c6d-1f2e3a4b5c6d";
const char* CHR_SETTINGS  = "b1c50001-9e0f-4b7a-9c6d-1f2e3a4b5c6d";
const char* CHR_STATUS    = "b1c50002-9e0f-4b7a-9c6d-1f2e3a4b5c6d";
const char* CHR_ROUTE     = "b1c50003-9e0f-4b7a-9c6d-1f2e3a4b5c6d";
const char* CHR_RIDES     = "b1c50004-9e0f-4b7a-9c6d-1f2e3a4b5c6d";
const char* CHR_OTA       = "b1c50005-9e0f-4b7a-9c6d-1f2e3a4b5c6d";
const char* CHR_SENSORS   = "b1c50006-9e0f-4b7a-9c6d-1f2e3a4b5c6d";
const char* CHR_MAP       = "b1c50007-9e0f-4b7a-9c6d-1f2e3a4b5c6d";
const char* CHR_AGNSS     = "b1c50008-9e0f-4b7a-9c6d-1f2e3a4b5c6d";
const char* CHR_DASH      = "b1c50009-9e0f-4b7a-9c6d-1f2e3a4b5c6d";
const char* CHR_MESH      = "b1c5000a-9e0f-4b7a-9c6d-1f2e3a4b5c6d";
const char* CHR_MEDIA     = "b1c5000b-9e0f-4b7a-9c6d-1f2e3a4b5c6d";
const char* CHR_WORKOUT   = "b1c5000c-9e0f-4b7a-9c6d-1f2e3a4b5c6d";

NimBLECharacteristic* statusChr = nullptr;
NimBLECharacteristic* sensorsChr = nullptr;
NimBLECharacteristic* ridesChr = nullptr;
NimBLECharacteristic* otaChr = nullptr;

// Bulk traffic keeps the link fast. Small location/metadata/media messages
// do not; GPS acquisition must never pin an otherwise quiet link at 30 ms.
std::atomic<uint32_t> lastBulkMs{0};
std::atomic<bool> intervalReportPending{false};
inline void noteBulk() { lastBulkMs = millis(); }
void servicePhoneInterval(); // BLE task only, including long outgoing streams

// Return code means locally queued, not accepted by the phone. Read back the
// actual parameters separately, including central-initiated changes.
int requestPhoneInterval(uint16_t handle, bool slow, const char* reason) {
    ble_gap_upd_params params = {};
    params.itvl_min = slow ? 120 : 12;
    params.itvl_max = slow ? 240 : 24;
    params.latency = 0;
    params.supervision_timeout = 400;
    params.min_ce_len = BLE_GAP_INITIAL_CONN_MIN_CE_LEN;
    params.max_ce_len = BLE_GAP_INITIAL_CONN_MAX_CE_LEN;
    int rc = ble_gap_update_params(handle, &params);
    diag::log("ble interval request: %s min=%.1f max=%.1fms latency=0 timeout=4000ms rc=%d (%s)",
              reason, params.itvl_min * 1.25f, params.itvl_max * 1.25f, rc,
              rc == 0 ? "queued; awaiting measured update" : NimBLEUtils::returnCodeToString(rc));
    return rc;
}

// Actual interval, not requested state. The legacy sleep gate requires at
// least 100 ms; MAIN_XTAL builds can maintain the phone at either interval.
// Refreshed by the BLE task, including during long outgoing transfers.
volatile bool linkIntervalLong = false;

// Legacy internal-RC clock tests lost the phone even at 150–300 ms (2026-08-19).
// Keep the per-boot experiment and three-timeout fallback. MAIN_XTAL builds
// below opt in after the private controller adapter fixes the sleep clock.
volatile int relaxedDropCount = 0;
#ifdef PM_BLE_XTAL
// Separate test build: the matching IDF controller adapter retains MAIN_XTAL.
// Automatic sleep may preserve the phone at any negotiated interval; the
// timeout latch below still falls back to the awake policy for this boot.
volatile bool relaxedSleepOk = true;
#else
volatile bool relaxedSleepOk = false;
#endif
volatile bool otaRebootPending = false;
SemaphoreHandle_t sharedLocMutex = nullptr;
volatile bool phoneClockSet = false;   // applyPhoneLocation set the system clock    // ble_shared::applyPhoneLocation
SemaphoreHandle_t sharedTileMutex = nullptr;   // ble_shared::lockTileList
SemaphoreHandle_t sharedDashMutex = nullptr;   // ble_shared::applyDash

NimBLECharacteristic* settingsChr = nullptr;

// Settings payload (little-endian), mirrored both ways so the app and device
// stay in sync:
//   [0] int16 ftpW   [2] int16 tzMin   [4] u8 useMiles   [5] u8 backlight
//   [6] u8 clock24h  [7] u8 usbDrive   [8] u16 wheel circumference, mm
// Grown by appending only: a write applies only the fields it is long enough
// to carry, so an older app (8 bytes) leaves the wheel size alone, and an
// app only sends the wheel size once it has READ one (i.e. this firmware).
void writeSettingsValue(NimBLECharacteristic* c) {
    uint8_t buf[10];
    int16_t ftp = (int16_t)settings::ftpWatts();
    int16_t tz = (int16_t)settings::tzMinutes();
    memcpy(buf, &ftp, 2);
    memcpy(buf + 2, &tz, 2);
    buf[4] = settings::useMiles() ? 1 : 0;
    buf[5] = (uint8_t)settings::backlight();
    buf[6] = settings::clock24h() ? 1 : 0;
    buf[7] = settings::usbDrive() ? 1 : 0;
    const uint16_t wheel = (uint16_t)settings::wheelCircMm();
    memcpy(buf + 8, &wheel, 2);
    c->setValue(buf, sizeof(buf));
}

// Set when a device-side settings change should be pushed to the phone.
volatile bool settingsDirty = false;
// Set when the layout changed, so the UI task repaints with it.
volatile bool dashDirty = false;

class SettingsCb : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        writeSettingsValue(c);
    }
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        std::string v = c->getValue();
        // [0xFE 0x55]: the paired phone unpairs itself (the app's "Unpair"
        // button) — same as Paired Devices > Phone > UNPAIR on the panel.
        // Settings is WRITE_AUTHEN, so only the paired phone gets here. Too short
        // to be a settings payload (>= 4 bytes), and older firmware drops it.
        if (v.size() == 2 && (uint8_t)v[0] == 0xFE && (uint8_t)v[1] == 0x55) {
            diag::log("ble: unpair requested by the paired phone");
            ble_server::requestUnpair();
            return;
        }
        if (v.size() >= 4) {
            ble_shared::SettingsPatch p;
            p.hasFtp = true;
            p.ftpW = (int16_t)((uint8_t)v[0] | ((uint8_t)v[1] << 8));
            p.hasTz = true;
            p.tzMin = (int16_t)((uint8_t)v[2] | ((uint8_t)v[3] << 8));
            if (v.size() >= 6) {
                p.hasMiles = true;
                p.useMiles = v[4] != 0;
                p.hasBacklight = true;
                p.backlight = (uint8_t)v[5];
            }
            if (v.size() >= 7) { p.hasClock24 = true; p.clock24h = v[6] != 0; }
            if (v.size() >= 8) { p.hasUsbDrive = true; p.usbDrive = v[7] != 0; }
            if (v.size() >= 10) {
                p.hasWheel = true;
                p.wheelMm = (uint8_t)v[8] | ((uint8_t)v[9] << 8);
            }
            ble_shared::applySettings(p);
        }
    }
};

// Dashboard layout. The value IS the config file's text — the same bytes that
// sit at /config/dashboard.cfg — so the app, the card and the panel can never
// drift into three different ideas of the layout. It is small enough (~300
// bytes) to move in a single read/write at the 247-byte MTU we negotiate, which
// is why this needs none of the chunked framing the route and map transfers use.
NimBLECharacteristic* dashChr = nullptr;

// Staged here by the write callback and applied by the server task.
//
// LOAD-BEARING, twice over. The first version did the work inline in onWrite()
// and reset the device on every send:
//
//   * it put two 1 KB buffers on the NimBLE callback stack, which is nowhere
//     near big enough for them — that alone is the reset;
//   * and it wrote the SD card from inside a BLE callback, which this file
//     already knows not to do. otaCommit and mapCommit are deferred to the task
//     for exactly this reason, and the deferral is also what keeps a write off
//     the card while the recorder or a USB host owns it.
//
// So: copy the bytes, set a flag, get out. Static rather than stack because
// this is a BLE callback, and sized for DASH_MAX_ITEMS worth of text.
constexpr size_t DASH_TEXT_MAX = 2048;   // sized with dash_config's TEXT_MAX
char dashPendingText[DASH_TEXT_MAX];
volatile bool dashApplyPending = false;

// Serialize the CURRENT layout into the characteristic. Static buffer, same
// reason — onRead runs on the BLE task too.
void writeDashValue(NimBLECharacteristic* c) {
    static char text[DASH_TEXT_MAX];
    size_t n = dash_config::currentText(text, sizeof(text));
    c->setValue((const uint8_t*)text, n);
}

// A multi-page config outgrew single BLE writes (iOS caps one write at 512
// bytes), so the app streams it: [0x01] begin, [0x02 bytes...] chunks,
// [0x03] commit. A write starting with a printable byte is the legacy
// whole-text path, kept so an older app's one-page config still lands.
size_t dashRxLen = 0;

class DashCb : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        writeDashValue(c);
    }
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        std::string v = c->getValue();
        if (v.empty()) return;
        const uint8_t op = (uint8_t)v[0];
        if (op == 0x01) {
            dashRxLen = 0;
        } else if (op == 0x02) {
            size_t n = v.size() - 1;
            if (dashRxLen + n >= DASH_TEXT_MAX) n = DASH_TEXT_MAX - 1 - dashRxLen;
            memcpy(dashPendingText + dashRxLen, v.data() + 1, n);
            dashRxLen += n;
        } else if (op == 0x03) {
            dashPendingText[dashRxLen] = 0;
            dashRxLen = 0;
            dashApplyPending = true;
        } else if (op >= 0x20 && v.size() < DASH_TEXT_MAX) {   // legacy text
            memcpy(dashPendingText, v.data(), v.size());
            dashPendingText[v.size()] = 0;
            dashApplyPending = true;
        }
    }
};

// --- Phone media (MUSIC page) -----------------------------------------------
// Phone -> device, one packet per write:
//   [0x01] meta : [u8 flags: bit0 playing][u16 posSec][u16 durSec]
//                 [title\0artist\0album\0]   (UTF-8, device truncates)
//   [0x10] art begin : [u16 w][u16 h] — 8-bit grayscale, w*h bytes follow
//   [0x13] tone art begin : [u16 w][u16 h] — tone indices already dithered
//                    on the phone, base-5 packed 3 px/byte (media.h)
//   [0x11] art data  : raw bytes, appended in order
//   [0x12] art end   : device dithers/unpacks + repaints (server task)
//   [0x02] clear     : no player / playback ended
//   [0x04] remote    : [u8 MediaCmd] — the app's MUSIC page buttons, sent on
//                      to iOS over AMS exactly like the device's own taps (an
//                      app can only drive the Music app; AMS drives any player).
//   [0x03] caps?     : device notifies [0xA1][u8 caps]. Older firmware never
//                      answers, which tells the app to stay on 8-bit art.
// Device -> phone (notify): [0xA0][MediaCmd] — play/pause, next, prev.
//                           [0xA1][caps]    — bit0: accepts 0x13 tone art;
//                                             bit1: forwards AMS tracks (0xA2);
//                                             bit2: takes [0x04] remote commands.
//                           [0xA2][title\0artist\0album\0] — the track AMS
//                              reports, each field cut to 56 bytes. Sent only
//                              to an app that asked [0x03] on this link (so an
//                              app that predates it never sees one): once on
//                              the ask, then on every track change. An iPhone
//                              app can't see other players' now-playing info,
//                              so this is how it learns whose art to fetch.
NimBLECharacteristic* mediaChr = nullptr;
volatile uint8_t mediaCmdPending = 0;    // MediaCmd queued by the UI task
volatile bool mediaArtPending = false;   // art fully received -> dither in task
volatile bool mediaCapsPending = false;  // caps asked -> notify from the task
volatile bool mediaTrackFeed = false;    // this link's app asked caps -> 0xA2
constexpr uint8_t MEDIA_CAP_TONE_ART = 0x01;
constexpr uint8_t MEDIA_CAP_AMS_TRACK = 0x02;
constexpr uint8_t MEDIA_CAP_REMOTE = 0x04;
constexpr size_t MEDIA_TRACK_FIELD_MAX = 56;   // 3 fields fit a 185 MTU notify

// Copy at most `cap` bytes of `src` without splitting a UTF-8 sequence, plus
// the NUL. Returns bytes written including the NUL.
size_t putTrackField(uint8_t* dst, const char* src, size_t cap) {
    size_t n = strnlen(src, cap);
    if (src[n] != 0)   // cut: back off any continuation bytes at the end
        while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) --n;
    memcpy(dst, src, n);
    dst[n] = 0;
    return n + 1;
}

class MediaCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        std::string v = c->getValue();
        if (v.empty()) return;
        const uint8_t* p = (const uint8_t*)v.data();
        size_t n = v.size();
        switch (p[0]) {
        case 0x01: {
            if (n < 6) return;
            bool playing = p[1] & 1;
            uint16_t pos = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
            uint16_t dur = (uint16_t)p[4] | ((uint16_t)p[5] << 8);
            // Three NUL-terminated strings; missing ones read as empty. Copied
            // into a bounded scratch with a forced terminator, so a packet
            // without NULs cannot run off the end.
            char text[200];
            size_t tl = n - 6 < sizeof(text) - 1 ? n - 6 : sizeof(text) - 1;
            memcpy(text, p + 6, tl);
            text[tl] = 0;
            const char* f[3] = {"", "", ""};
            size_t off = 0;
            for (int i = 0; i < 3 && off <= tl; ++i) {
                f[i] = text + off;
                off += strlen(text + off) + 1;
            }
            media::setMeta(playing, pos, dur, f[0], f[1], f[2]);
            break;
        }
        // Art is up to ~90 KB: keep the link at the bulk interval while it
        // flows, like route and tile uploads. Left to the idle interval it
        // crawled along at 150-300 ms per connection event.
        case 0x10:
        case 0x13:
            noteBulk();
            if (n >= 5) {
                int w = (int)(p[1] | (p[2] << 8)), h = (int)(p[3] | (p[4] << 8));
                if (p[0] == 0x13) media::beginToneArt(w, h);
                else media::beginArt(w, h);
            }
            break;
        case 0x11:
            noteBulk();
            media::artData(p + 1, n - 1);
            break;
        case 0x12:
            // Dithering ~100 KB is server-task work, not callback work — the
            // same deferral otaCommit and mapCommit already use.
            mediaArtPending = true;
            break;
        case 0x02:
            media::clearFromApp();
            break;
        case 0x03:
            mediaCapsPending = true;
            break;
        case 0x04:
            // Queued for the server task, the same hand-off the UI task's
            // taps use: AMS writes don't belong in a GATT callback.
            if (n >= 2 && p[1] >= MC_TOGGLE && p[1] <= MC_VOL_DOWN) mediaCmdPending = p[1];
            break;
        }
    }
};

// Route upload framing (each BLE write is one packet):
//   [0x01] start     : rest is the route name (UTF-8)
//   [0x02] data      : rest is raw GPX bytes, appended in order
//   [0x03] end track : finalize geometry -> parse + activate
//   [0x04] maneuver  : [i32 lat_e7][i32 lon_e7][utf8 instruction]
//   [0x05] end nav   : all maneuvers received -> raise nav prompt, then the
//                      device notifies [0x23] received-ok / [0x24] parse-failed
//                      so the phone can confirm receipt instead of guessing
//   [0x06] list      : device notifies [0x20][name].. then [0x21] done
//   [0x07] delete    : rest is a route filename to remove
//   [0x08] location  : the phone's live fix. Fields are positional and every
//                      one past lat/lon is optional, so old apps and old
//                      firmware interoperate (each side ignores what it lacks):
//                        [i32 lat_e7][i32 lon_e7][u32 utc s][i16 alt m]
//                        [i16 h-accuracy m][u16 speed cm/s][u16 course 0.01 deg]
//                      speed/course 0xFFFF = the phone has no valid value
//                      (iOS reports -1 standing still; Android may lack them).
constexpr size_t ROUTE_MAX = 256 * 1024;  // 256 KB GPX cap (PSRAM)
char* routeBuf = nullptr;
size_t routeLen = 0;
char routeName[40] = "route.gpx";

NimBLECharacteristic* routeChr = nullptr;
enum RouteReq { RREQ_NONE, RREQ_LIST, RREQ_DELETE };
volatile RouteReq routeReq = RREQ_NONE;
char routeReqName[48];

// Receipt acknowledgement for the phone. onWrite can't safely notify from the
// BLE callback, so 0x05 stashes the outcome here and the server loop notifies.
volatile bool routeTrackLoaded = false;   // did the last 0x03 track parse OK
volatile int  pendingRouteAck  = 0;       // 0 none, 1 received-ok, 2 parse-failed

class RouteCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        noteBulk();   // route upload chunks — keep the link fast while they flow
        std::string v = c->getValue();
        if (v.empty()) return;
        uint8_t op = v[0];
        const char* payload = v.data() + 1;
        size_t plen = v.size() - 1;

        if (op == 0x01) {  // start
            routeLen = 0;
            routeTrackLoaded = false;
            routes::clearManeuvers();
            size_t n = plen < sizeof(routeName) - 1 ? plen : sizeof(routeName) - 1;
            memcpy(routeName, payload, n);
            routeName[n] = 0;
            Serial.printf("[srv] route upload start: %s\n", routeName);
        } else if (op == 0x02) {  // data
            if (routeBuf && routeLen + plen <= ROUTE_MAX) {
                memcpy(routeBuf + routeLen, payload, plen);
                routeLen += plen;
            }
        } else if (op == 0x03) {  // end track
            if (routeBuf && routeLen > 0) {
                routeBuf[routeLen < ROUTE_MAX ? routeLen : ROUTE_MAX - 1] = 0;
                bool ok = routes::loadFromMemory(routeName, routeBuf, routeLen);
                routeTrackLoaded = ok;
                Serial.printf("[srv] route track end: %u bytes, %s\n",
                              (unsigned)routeLen, ok ? "loaded" : "parse failed");
            }
        } else if (op == 0x04 && plen >= 8) {  // maneuver
            int32_t latE7, lonE7;
            memcpy(&latE7, payload, 4);
            memcpy(&lonE7, payload + 4, 4);
            char instr[routes::MANEUVER_TEXT];
            size_t tn = plen - 8;
            if (tn > sizeof(instr) - 1) tn = sizeof(instr) - 1;
            memcpy(instr, payload + 8, tn);
            instr[tn] = 0;
            routes::addManeuver(latE7 / 1e7, lonE7 / 1e7, instr);
        } else if (op == 0x05) {  // end nav
            routes::finishManeuvers();
            Serial.printf("[srv] navigation ready: %d maneuvers\n",
                          routes::maneuverCount());
            pendingRouteAck = routeTrackLoaded ? 1 : 2;   // tell the phone
        } else if (op == 0x06) {  // list routes
            routeReq = RREQ_LIST;
        } else if (op == 0x07 && plen > 0) {  // delete route
            size_t n = plen < sizeof(routeReqName) - 1 ? plen
                                                       : sizeof(routeReqName) - 1;
            memcpy(routeReqName, payload, n);
            routeReqName[n] = 0;
            routeReq = RREQ_DELETE;
        } else if (op == 0x08 && plen >= 8) {  // phone's live location
            ble_shared::PhoneFix fx;
            memcpy(&fx.latE7, payload, 4);
            memcpy(&fx.lonE7, payload + 4, 4);
            if (plen >= 12) { memcpy(&fx.utc, payload + 8, 4); fx.haveUtc = true; }
            if (plen >= 14) { memcpy(&fx.altM, payload + 12, 2); fx.haveAlt = true; }
            // The app has always sent its horizontal accuracy here; we used to
            // stop parsing before it. The recorder needs it to decide whether a
            // phone position is good enough to go in the ride file.
            if (plen >= 16) memcpy(&fx.accM, payload + 14, 2);
            // Speed + course (newer apps). Absent or 0xFFFF: derive instead.
            if (plen >= 18) {
                uint16_t v16;
                memcpy(&v16, payload + 16, 2);
                if (v16 != 0xFFFF) fx.speedMs = v16 / 100.0f;
            }
            if (plen >= 20) {
                uint16_t v16;
                memcpy(&v16, payload + 18, 2);
                if (v16 != 0xFFFF && v16 < 36000) fx.courseDeg = v16 / 100.0f;
            }
            ble_shared::applyPhoneLocation(fx);
        }
    }
};

// Structured workouts (phone <-> device). The app uploads .erg/.mrc files,
// starts/stops the session and reads live progress.
//   Phone -> device:  [0x01]<name> begin upload   [0x02]<bytes> chunk
//                     [0x03] end (save to /workouts + load)
//                     [0x06] list files           [0x07]<name> delete
//                     [0x11]<name> load           [0x30] status request
//                     [0x12]<name> fetch file (for the app's editor)
//                     [0x20] start [0x21] pause [0x22] resume [0x23] stop
//                     [0x24] skip  [0x25]<u8 idx> start block idx
//                     [0x26]<u8 0|1> pause after every block
//                     [0x27] stop + unload (page returns to no-workout)
//   Device -> phone (notify):
//     [0xA0][1 ok|2 fail]<utf8 msg>   upload/load/delete outcome
//     [0xB0] list begin  [0xB1]<name> per file  [0xB2] list end
//     [0xD1]<bytes> fetched-file chunk  [0xD2] fetch end (0xA0 fail if missing)
//     [0xC0][loaded][running][paused][done][segIdx][segCount]
//           [elapsed u32][total u32][target u16][ftp u16][pauseEach]<utf8 name>
// File writes and directory scans run in the server task, not the BLE
// callback (same discipline as routes/rides — SD work wedges the host task).
NimBLECharacteristic* workoutChr = nullptr;
enum WorkoutReq { WREQ_NONE, WREQ_DELETE, WREQ_LOAD, WREQ_FETCH };
volatile WorkoutReq workoutReq = WREQ_NONE;   // the name-carrying ops
volatile bool workoutSavePending = false;     // 0x03 — end of an upload
volatile bool workoutListPending = false;     // 0x06 — may follow 0x03 at once
char workoutReqName[48];
constexpr size_t WORKOUT_UPLOAD_MAX = 16 * 1024;
char* workoutUpBuf = nullptr;
volatile size_t workoutUpLen = 0;
volatile bool workoutStatusDirty = false;

class WorkoutCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        std::string v = c->getValue();
        if (v.empty()) return;
        uint8_t op = v[0];
        const char* payload = v.data() + 1;
        size_t plen = v.size() - 1;
        if (op == 0x01) {              // begin upload
            noteBulk();
            if (!workoutUpBuf)
                workoutUpBuf = (char*)heap_caps_malloc(WORKOUT_UPLOAD_MAX,
                                                       MALLOC_CAP_SPIRAM);
            workoutUpLen = 0;
            size_t n = plen < sizeof(workoutReqName) - 1
                           ? plen : sizeof(workoutReqName) - 1;
            memcpy(workoutReqName, payload, n);
            workoutReqName[n] = 0;
            Serial.printf("[srv] workout upload start: %s\n", workoutReqName);
        } else if (op == 0x02) {       // chunk
            noteBulk();
            if (workoutUpBuf && workoutUpLen + plen <= WORKOUT_UPLOAD_MAX) {
                memcpy(workoutUpBuf + workoutUpLen, payload, plen);
                workoutUpLen += plen;
            }
        } else if (op == 0x03) {       // end -> save + load in the task
            workoutSavePending = true;
        } else if (op == 0x06) {
            workoutListPending = true;
        } else if ((op == 0x07 || op == 0x11 || op == 0x12) && plen > 0) {
            size_t n = plen < sizeof(workoutReqName) - 1
                           ? plen : sizeof(workoutReqName) - 1;
            memcpy(workoutReqName, payload, n);
            workoutReqName[n] = 0;
            workoutReq = op == 0x07   ? WREQ_DELETE
                         : op == 0x11 ? WREQ_LOAD
                                      : WREQ_FETCH;
        } else if (op == 0x20) { workout_service::start();  workoutStatusDirty = true; }
        else if (op == 0x21) { workout_service::pause();  workoutStatusDirty = true; }
        else if (op == 0x22) { workout_service::resume(); workoutStatusDirty = true; }
        else if (op == 0x23) { workout_service::stop();   workoutStatusDirty = true; }
        else if (op == 0x24) { workout_service::skip();   workoutStatusDirty = true; }
        else if (op == 0x25 && plen >= 1) {
            workout_service::jumpToSeg((int)(uint8_t)payload[0]);
            workoutStatusDirty = true;
        } else if (op == 0x26 && plen >= 1) {
            settings::setWorkoutPauseEachBlock(payload[0] != 0);
            workoutStatusDirty = true;
        } else if (op == 0x27) {
            workout_service::unload();
            workoutStatusDirty = true;
        } else if (op == 0x30) {
            workoutStatusDirty = true;
        }
    }
};

// Sensor management (phone <-> device). Lets the app scan for, pair, and
// forget the head unit's cycling sensors and see their live connection state.
//   Phone -> device:  [0x01] start scan + stream   [0x02] stop scan
//                     [0x03]<addr> pair            [0x04]<addr> forget
//                     [0x05] request list once
//   Device -> phone (notify):
//     [0x10] list begin
//     [0x11][kindsMask][flags][rssi:i8][nameLen][name…][addr(17)]  per sensor
//              flags bit0=connected bit1=paired
//     [0x12] list end
enum SensorReq { SREQ_NONE, SREQ_LIST };
volatile SensorReq sensorReq = SREQ_NONE;
volatile bool sensorsStreaming = false;   // app has the sensors screen open
char sensorReqAddr[20];

class SensorsCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        std::string v = c->getValue();
        if (v.empty()) return;
        uint8_t op = v[0];
        if (op == 0x01) {                    // start scan + live stream
            ble_sensors::setScanAlways(true);
            ble_sensors::noteActivity();
            sensorsStreaming = true;
            sensorReq = SREQ_LIST;
        } else if (op == 0x02) {             // stop scan
            ble_sensors::setScanAlways(false);
            sensorsStreaming = false;
        } else if (op == 0x03 && v.size() > 1) {   // pair address
            size_t n = v.size() - 1 < sizeof(sensorReqAddr) - 1 ? v.size() - 1
                                                                : sizeof(sensorReqAddr) - 1;
            memcpy(sensorReqAddr, v.data() + 1, n);
            sensorReqAddr[n] = 0;
            ble_sensors::pairCandidate(sensorReqAddr);
            sensorReq = SREQ_LIST;
        } else if (op == 0x04 && v.size() > 1) {   // forget address
            size_t n = v.size() - 1 < sizeof(sensorReqAddr) - 1 ? v.size() - 1
                                                                : sizeof(sensorReqAddr) - 1;
            memcpy(sensorReqAddr, v.data() + 1, n);
            sensorReqAddr[n] = 0;
            ble_sensors::forget(sensorReqAddr);
            sensorReq = SREQ_LIST;
        } else if (op == 0x05) {             // request one snapshot
            sensorReq = SREQ_LIST;
        }
    }
};

static void sendSensorList() {
    if (!sensorsChr) return;
    ble_sensors::Candidate cs[10];
    int n = ble_sensors::getCandidates(cs, 10);
    uint8_t begin = 0x10;
    sensorsChr->setValue(&begin, 1);
    sensorsChr->notify();
    for (int i = 0; i < n; ++i) {
        uint8_t pkt[64];
        int nameLen = (int)strnlen(cs[i].name, sizeof(cs[i].name));
        if (nameLen > 31) nameLen = 31;
        int p = 0;
        pkt[p++] = 0x11;
        pkt[p++] = cs[i].kindsMask;
        pkt[p++] = (cs[i].connected ? 1 : 0) | (cs[i].paired ? 2 : 0);
        pkt[p++] = (uint8_t)cs[i].rssi;
        pkt[p++] = (uint8_t)nameLen;
        memcpy(pkt + p, cs[i].name, nameLen); p += nameLen;
        int addrLen = (int)strnlen(cs[i].addr, sizeof(cs[i].addr));
        memcpy(pkt + p, cs[i].addr, addrLen); p += addrLen;
        sensorsChr->setValue(pkt, p);
        sensorsChr->notify();
    }
    uint8_t end = 0x12;
    sensorsChr->setValue(&end, 1);
    sensorsChr->notify();
}

// Ride download (device -> phone). The phone writes a command; the task
// streams the response via notifications on the same characteristic.
//   Phone -> device:  [0x01] list  [0x02]<name> download  [0x03]<name> delete
//   Device -> phone:  list:     [0x01][u32 size][name] per ride, [0x03] end
//                     download:  [0x10][u32 total], [0x11]<bytes>…, [0x12] end
//                     delete ack: [0x13]        error (e.g. recording): [0x1F]
enum RideReq { REQ_NONE, REQ_LIST, REQ_DOWNLOAD, REQ_DELETE, REQ_LOG,
               REQ_LOG_LIST, REQ_LOG_FILE };
volatile RideReq rideReq = REQ_NONE;
char rideReqName[48];

// Windowed-transfer ack from the phone: [0x04][u16 nextExpectedSeq].
volatile bool rideAckPending = false;
volatile uint16_t rideAckSeq = 0;

class RidesCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        noteBulk();   // download requests AND per-window acks land here
        std::string v = c->getValue();
        if (v.empty()) return;
        if (v[0] == 0x01) {
            rideReq = REQ_LIST;
        } else if (v[0] == 0x05) {                    // download today's log
            rideReq = REQ_LOG;
        } else if (v[0] == 0x06) {                    // list per-day log files
            rideReq = REQ_LOG_LIST;
        } else if (v[0] == 0x04 && v.size() >= 3) {   // window ack
            rideAckSeq = (uint8_t)v[1] | ((uint8_t)v[2] << 8);
            rideAckPending = true;
        } else if ((v[0] == 0x02 || v[0] == 0x03 || v[0] == 0x07) && v.size() > 1) {
            size_t n = v.size() - 1;
            if (n > sizeof(rideReqName) - 1) n = sizeof(rideReqName) - 1;
            memcpy(rideReqName, v.data() + 1, n);
            rideReqName[n] = 0;
            rideReq = v[0] == 0x02 ? REQ_DOWNLOAD
                    : v[0] == 0x03 ? REQ_DELETE : REQ_LOG_FILE;   // 0x07 = one log file
        }
    }
};

void notifyByte(uint8_t b) {
    ridesChr->setValue(&b, 1);
    ridesChr->notify();
}

void notifyByte2(uint8_t b) {   // on the route characteristic
    routeChr->setValue(&b, 1);
    routeChr->notify();
}

// Negotiated ATT MTU (updated by ServerCb). notify() payloads above
// (MTU - 3) get truncated, so chunks are sized to fit.
volatile uint16_t negotiatedMTU = 23;
// True only while an ADMITTED phone is on the link (see "Fixed pairing"
// below): a central that has connected but not yet proven it holds our bond
// is a link, not a phone, and gets no pushes, no fast interval and no status
// bar icon.
volatile bool phoneConnected = false;   // updated by ServerCb
std::atomic<uint32_t> phoneConnectionEpoch{0};

// ---------------------------------------------------------------------------
// Fixed pairing.
//
// The device belongs to one phone, and everything the app can touch goes over
// a link that phone AUTHENTICATED. Mechanism, and why it is this one:
//
//  * Every app characteristic is READ_AUTHEN/WRITE_AUTHEN, and so is every
//    CCCD (ble_pair_guard_harden: the host creates those, and left them open,
//    so an unencrypted central could subscribe). The host runs in Secure
//    Connections Only mode, which makes "authenticated" mean LE Secure
//    Connections + MITM (the passkey shown on the panel) + a 16-byte key. An
//    unauthenticated central reads, writes and subscribes to nothing; it gets
//    Insufficient Authentication, which is also what makes a phone start
//    pairing on an unpaired device.
//  * A link is ADMITTED (phoneConnected, pushes, the fast interval, the status
//    bar icon, AMS) only in onAuthenticationComplete, and only if it is
//    encrypted AND authenticated AND bonded AND the bond is the paired
//    phone's (or, unpaired, the new bond this pairing just made). Anything
//    else is dropped, and a bond it made is deleted. "Just Works" from a
//    no-IO peer never authenticates, so it can't own the device.
//  * While a phone bond exists ("paired"), the device refuses until the rider
//    unpairs — and refuses BEFORE anything that could put a pairing dialog on
//    a phone:
//      - an unrecognised central is disconnected in onConnect, before the
//        security request (that request is what made iOS start pairing and
//        show its passkey dialog while the panel showed nothing — then iOS
//        reconnected and asked again, every half second, 2026-10-10).
//      - a recognised identity that tries to PAIR again (the phone forgot us)
//        has the request ignored by ble_pair_guard before the pairing
//        response, and is dropped. NimBLE-Arduino would otherwise delete the
//        paired phone's bond on the spot and pair whoever asked.
//      - a recognised identity whose encryption fails (stale or mismatched
//        keys) is dropped, and its bond KEPT: deleting it would unpair the
//        device on behalf of whoever sent the bad keys.
//    The apps see the drop (and the paired flag in the advertisement) and
//    stop reconnecting, with a message: unpair on the device, then connect.
//    The paired phone never pairs again — its link encrypts with the stored
//    keys, silently, in ~100 ms.
//  * Advertising stays open to everyone. A controller whitelist would keep
//    strangers from even connecting, but phones connect from resolvable
//    private addresses that rotate every ~15 min, so the whitelist only works
//    if the controller's resolving list is populated and address resolution is
//    on — which nothing on this build verifies, and the failure mode is the
//    OWNER's phone silently locked out. Accept-then-verify can't fail that way:
//    the bond lookup that admits the phone is the same one that has always
//    resolved its RPA for AMS. A stranger costs the paired phone at most a few
//    seconds of a busy radio, and the apps skip a paired device when scanning
//    (manufacturer data below says which).
//  * "Paired" is simply "a phone bond exists" — the bond store is the record,
//    so there is no second copy to drift from it. Bonds made with a SENSOR
//    (the device is a central to those, and a sensor may ask for encryption)
//    are excluded by address. settings::phoneId/phoneName only remember who
//    the phone is for the panel.
//  * Migration: a device that already holds phone bonds from before this
//    change boots paired — every existing phone bond stays admitted until the
//    rider unpairs (which deletes them all). Phone bonds have been passkey
//    (MITM) pairings since phone bonding was added, so they authenticate.
//
// Unpaired, the first phone to connect and complete passkey pairing becomes
// the paired phone; the code is on the panel over whatever screen is up.

// Manufacturer data in the advertisement: [0xFFFF company id][0x4F 'O'][flags].
// flags bit0 = paired. 0xFFFF is the SIG's "no company / testing" id.
constexpr uint8_t ADV_MAGIC = 0x4F;
constexpr uint8_t ADV_FLAG_PAIRED = 0x01;

volatile bool linkUp = false;            // a central is connected (admitted or not)
volatile uint16_t linkHandle = 0xFFFF;
volatile bool linkAdmitted = false;
// The peer's identity already had a bond when it connected — i.e. it is a
// paired phone (or the paired phone re-pairing after it forgot us).
volatile bool linkPeerKnown = false;
// This link was refused while paired (see above): its drop is the refusal, not
// a flaky phone, and says nothing about stale keys.
volatile bool linkRefused = false;
volatile bool unpairPending = false;
volatile bool advDirty = false;          // pairing state changed; re-advertise
std::atomic<int> phoneBonds{0};          // cached; recomputed on every change
char phoneNameCache[32] = "";

bool isSensorAddr(const NimBLEAddress& a) {
    std::string s = a.toString();
    for (int k = 0; k < ble_sensors::KIND_COUNT; ++k)
        if (settings::sensorPaired(k, s.c_str())) return true;
    return false;
}

int countPhoneBonds() {
    int n = 0, total = NimBLEDevice::getNumBonds();
    for (int i = 0; i < total; ++i)
        if (!isSensorAddr(NimBLEDevice::getBondedAddress(i))) ++n;
    return n;
}

void recountPhoneBonds() {
    int before = phoneBonds.load();
    int now = countPhoneBonds();
    phoneBonds = now;
    if ((before > 0) != (now > 0)) advDirty = true;
}

// Rebuilds the advertisement from scratch (NimBLE's setters append rather than
// replace, so changing one byte means starting over) and starts advertising.
// Name goes in the scan response: flags + a 128-bit UUID + our 6-byte
// manufacturer field already take 27 of the 31 advertising bytes.
void applyAdvertising() {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->reset();
    adv->addServiceUUID(SVC_UUID);
    uint8_t mfg[4] = {0xFF, 0xFF, ADV_MAGIC,
                      (uint8_t)(phoneBonds.load() > 0 ? ADV_FLAG_PAIRED : 0)};
    adv->setManufacturerData(mfg, sizeof(mfg));
    adv->enableScanResponse(true);
    adv->setName("OpenTrailPaper");
    advDirty = false;
    NimBLEDevice::startAdvertising();
}

// Keep a name the panel's fonts can draw: iPhones default to
// "Raemond’s iPhone" with a typographic apostrophe, which has no glyph.
void foldName(const char* in, char* out, size_t cap) {
    size_t n = 0;
    for (const uint8_t* p = (const uint8_t*)in; *p && n + 1 < cap; ) {
        if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x98 || p[2] == 0x99)) {
            out[n++] = '\''; p += 3;           // ‘ ’
        } else if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x9C || p[2] == 0x9D)) {
            out[n++] = '"'; p += 3;            // “ ”
        } else if (*p >= 0x20 && *p < 0x7F) {
            out[n++] = (char)*p++;
        } else {
            ++p;                               // other UTF-8: drop the byte
        }
    }
    while (n && out[n - 1] == ' ') --n;
    out[n] = 0;
}

// Refuse a link while paired. Reason 0x05 (Authentication Failure) is what
// Android reports as the disconnect status, so the app can tell a refusal from
// a drop. A stranger's phone may retry every half second for as long as its
// app is open, so the log line is rate-limited.
void kickLink(uint16_t handle, const char* why, bool phoneSlot = true) {
    static uint32_t lastLogMs = 0, suppressed = 0;
    if (phoneSlot) linkRefused = true;
    if (lastLogMs == 0 || millis() - lastLogMs > 30000) {
        diag::log("ble: refusing phone link (%s) — the device is paired with "
                  "another phone; unpair it in Paired Devices > Phone "
                  "(%lu more refusal(s) not logged)", why, (unsigned long)suppressed);
        lastLogMs = millis();
        suppressed = 0;
    } else {
        ++suppressed;
    }
    NimBLEDevice::getServer()->disconnect(handle, BLE_ERR_AUTH_FAIL);
}

// ble_pair_guard asks this when a peer we hold a bond for sends a pairing
// request (NimBLE host task). Paired: always refuse — it is the paired
// phone's identity having forgotten us, or someone presenting that identity,
// and either way the answer is "unpair on the device first". Unpaired the
// only bonds are sensors', and a sensor is never the central on this server.
bool refuseRepeatPairing(uint16_t handle) {
    if (phoneBonds.load() == 0) return false;
    kickLink(handle, "the paired phone tried to pair again — it forgot this "
                     "device");
    return true;
}

// Server task: delete every phone bond and reopen pairing.
//
// Phones hand over an IRK when they pair, and ble_gap_unpair will not delete a
// bond with an IRK while advertising or scanning is on (BLE_HS_EBUSY: the
// controller's resolving list can't change then) — it returns without
// touching anything. That made UNPAIR a no-op whenever a stranger's refusals
// had advertising running or a sensor hunt was scanning: the phone's bond
// survived and it reconnected straight away (2026-10-10). So both stop first,
// every delete is checked, and an unpair that didn't take stays pending and
// is retried — the panel already reads "not paired" while it is.
void serviceUnpair() {
    if (!unpairPending) return;
    static uint8_t tries = 0;
    ble_sensors::holdScan(true);
    NimBLEDevice::stopAdvertising();
    int total = NimBLEDevice::getNumBonds();
    int deleted = 0, failed = 0, lastRc = 0;
    for (int i = total - 1; i >= 0; --i) {   // from the end: deletion shifts
        NimBLEAddress a = NimBLEDevice::getBondedAddress(i);
        if (isSensorAddr(a)) continue;
        // ble_gap_unpair also terminates a live link to that peer.
        int rc = ble_gap_unpair(a.getBase());
        if (rc == 0) ++deleted;
        else { ++failed; lastRc = rc; }
    }
    if (countPhoneBonds() > 0) {
        if (++tries < 20) {
            if (tries == 1)
                diag::log("ble: unpair: %d phone bond(s) not deleted yet (rc=%d) "
                          "— retrying", failed, lastRc);
            return;   // still pending; advertising and scanning stay off
        }
        diag::log("ble: unpair FAILED — %d phone bond(s) left (rc=%d); "
                  "try again from Paired Devices > Phone", countPhoneBonds(), lastRc);
    }
    tries = 0;
    ble_sensors::holdScan(false);
    settings::clearPhone();
    phoneNameCache[0] = 0;
    if (linkUp) NimBLEDevice::getServer()->disconnect(linkHandle);
    recountPhoneBonds();
    advDirty = true;
    unpairPending = false;
    if (phoneBonds.load() == 0)
        diag::log("ble: unpaired from the panel — %d phone bond(s) deleted, "
                  "pairing open to the next phone", deleted);
    // With a link up, onDisconnect re-advertises; with none, do it now.
    if (!linkUp) applyAdvertising();
}

// Best-effort paced notify. Returns false if notify() never succeeded
// (the packet was NOT delivered). Never aborts the transfer.
bool sendChunk(NimBLECharacteristic* chr, const uint8_t* data, size_t len) {
    noteBulk();
    servicePhoneInterval(); // do not wait for a long transfer to finish to speed up
    chr->setValue(data, len);
    bool ok = false;
    for (int r = 0; r < 40; ++r) {
        if (chr->notify()) { ok = true; break; }
        vTaskDelay(pdMS_TO_TICKS(8));
    }
    vTaskDelay(pdMS_TO_TICKS(10));  // pacing so the buffer can drain
    return ok;
}

void sendRideList() {
    if (ride_recorder::isRecording() || !ride_recorder::sdMounted()) {
        notifyByte(0x1F);
        return;
    }
    ble_shared::listDir(ble_shared::Dir::Rides, [](const char* base, uint32_t sz, void*) {
        uint8_t pkt[64];
        pkt[0] = 0x01;
        memcpy(pkt + 1, &sz, 4);
        size_t nl = strlen(base);
        if (nl > sizeof(pkt) - 5) nl = sizeof(pkt) - 5;
        memcpy(pkt + 5, base, nl);
        sendChunk(ridesChr, pkt, 5 + nl);
    }, nullptr);
    notifyByte(0x03);
}

// Stream any SD file to the phone with the reliable windowed protocol (used
// for both ride downloads and the diagnostics log).
void streamFileWindowed(const char* path, const char* label) {
    noteBulk();
    sdLock();
    File f = SD.open(path, FILE_READ);
    sdUnlock();
    if (!f) { notifyByte(0x1F); return; }

    uint32_t total = f.size();
    // Payload per chunk must leave room for [0x11][u16 seq] and fit the MTU.
    int chunk = (int)negotiatedMTU - 3 - 3;    // -3 ATT overhead, -3 our header
    if (chunk < 16) chunk = 16;
    if (chunk > 180) chunk = 180;
    diag::log("download %s: total=%u, MTU=%u, chunk=%d", label,
              (unsigned)total, negotiatedMTU, chunk);

    uint8_t hdr[5] = {0x10};
    memcpy(hdr + 1, &total, 4);
    sendChunk(ridesChr, hdr, 5);

    // Windowed transfer with acknowledgement + retransmission. Fire-and-forget
    // notify streaming is unreliable — iOS silently drops notifications that
    // arrive faster than the app consumes them. Instead we send a window of
    // WINDOW chunks, then wait for the app to write back the next sequence it
    // needs ([0x04][u16 seq]). If it's short of what we sent, a packet was
    // lost, so we seek back and resend from there. Each chunk carries a u16
    // seq the app uses to stay strictly in order.
    constexpr int WINDOW = 8;
    uint8_t pkt[184];
    pkt[0] = 0x11;
    uint16_t seq = 0;
    bool eof = false;
    int resends = 0;
    while (true) {
        if (!phoneConnected) {              // gave up / dropped mid-transfer
            diag::log("download %s: disconnected at seq %u", label, seq);
            f.close();
            return;
        }
        if (resends > 200) {                // a chunk keeps failing — bail, don't hang
            diag::log("download %s: too many resends, aborting", label);
            f.close();
            return;
        }
        sdLock();
        f.seek((uint32_t)seq * chunk);
        for (int i = 0; i < WINDOW; ++i) {
            int n = f.read(pkt + 3, chunk);
            if (n <= 0) { eof = true; break; }
            pkt[1] = seq & 0xFF;
            pkt[2] = seq >> 8;
            sendChunk(ridesChr, pkt, n + 3);
            seq++;
        }
        sdUnlock();
        // Ask the app what it has, then wait for its ack.
        rideAckPending = false;
        uint8_t we = 0x14;                 // window-end / please-ack
        sendChunk(ridesChr, &we, 1);
        uint32_t t0 = millis();
        while (!rideAckPending && phoneConnected && millis() - t0 < 5000)
            vTaskDelay(pdMS_TO_TICKS(5));
        if (!rideAckPending) {
            diag::log("download %s: ACK TIMEOUT at seq %u", label, seq);
            f.close();
            return;
        }
        if (rideAckSeq < seq) {             // a chunk was lost — resend from there
            resends++;
            seq = rideAckSeq;
            eof = false;
            continue;
        }
        if (eof) break;
    }
    f.close();
    uint8_t done = 0x12;
    sendChunk(ridesChr, &done, 1);
    diag::log("download %s: %u bytes, %d resends", label, (unsigned)total, resends);
}

void sendRide(const char* name) {
    if (ride_recorder::isRecording() || !ride_recorder::sdMounted()) {
        notifyByte(0x1F);
        return;
    }
    char path[80];
    snprintf(path, sizeof(path), RIDE_DIR "/%s", name);
    streamFileWindowed(path, name);
}

void sendLog() {
    if (ride_recorder::isRecording() || !ride_recorder::sdMounted()) {
        notifyByte(0x1F);
        return;
    }
    diag::flushToSD();                 // make sure the newest lines are on SD
    streamFileWindowed(diag::logPath(), "diag.log");
}

// List the per-day log files under /logs. Reply: [0x30][u32 size][name] per
// file, then [0x31] done ([0x1F] if SD busy).
void sendLogList() {
    if (ride_recorder::isRecording() || !ride_recorder::sdMounted()) {
        notifyByte(0x1F);
        return;
    }
    diag::flushToSD();                 // so today's file exists + is current
    ble_shared::listDir(ble_shared::Dir::Logs, [](const char* base, uint32_t sz, void*) {
        uint8_t pkt[64];
        pkt[0] = 0x30;
        memcpy(pkt + 1, &sz, 4);
        size_t nl = strlen(base);
        if (nl > sizeof(pkt) - 5) nl = sizeof(pkt) - 5;
        memcpy(pkt + 5, base, nl);
        sendChunk(ridesChr, pkt, 5 + nl);
    }, nullptr);
    notifyByte(0x31);
}

void sendLogFile(const char* name) {
    if (ride_recorder::isRecording() || !ride_recorder::sdMounted()) {
        notifyByte(0x1F);
        return;
    }
    diag::flushToSD();
    char path[64];
    // Match the 47-byte request-name capacity; Memfault export names are
    // 45 bytes. The old 40-byte limit cut off their suffix during download.
    snprintf(path, sizeof(path), "/logs/%.47s", name);
    streamFileWindowed(path, name);
}

void deleteRide(const char* name) {
    if (ride_recorder::isRecording()) { notifyByte(0x1F); return; }
    ble_shared::deleteRide(name);
    notifyByte(0x13);
}

void sendRouteList() {
    if (!ride_recorder::sdMounted()) { notifyByte(0x21); return; }
    const bool opened = ble_shared::listDir(ble_shared::Dir::Routes,
                                            [](const char* base, uint32_t, void*) {
        uint8_t pkt[64];
        pkt[0] = 0x20;
        size_t nl = strlen(base);
        if (nl > sizeof(pkt) - 1) nl = sizeof(pkt) - 1;
        memcpy(pkt + 1, base, nl);
        sendChunk(routeChr, pkt, 1 + nl);
    }, nullptr);
    // (A missing /routes has always answered on the rides characteristic.)
    if (!opened) notifyByte(0x21);
    else notifyByte2(0x21);
}

void deleteRoute(const char* name) {
    ble_shared::deleteRoute(name);
    notifyByte2(0x22);
}

// Runs before queued work and from paced download sends, so a long stream
// cannot starve the governor. Its mutable policy belongs to this task only.
void servicePhoneInterval() {
    static BleIntervalPolicy policy;
    static uint32_t epoch = 0, lastLog = 0;
    static bool hadPhone = false;
    static uint16_t lastInterval = 0, lastLatency = 0, lastTimeout = 0;
    bool report = intervalReportPending.exchange(false);
    if (!phoneConnected) {
        hadPhone = false; linkIntervalLong = false;
        if (report) diag::log("ble interval state: no phone connected");
        return;
    }
    NimBLEServer* srv = NimBLEDevice::getServer();
    if (!srv || !srv->getConnectedCount()) return;
    NimBLEConnInfo info = srv->getPeerInfo(0);
    uint32_t now = millis(), currentEpoch = phoneConnectionEpoch.load();
    if (!hadPhone || epoch != currentEpoch) {
        policy.reset(now); epoch = currentEpoch; hadPhone = true;
        lastInterval = lastLatency = lastTimeout = 0;
    }
    uint16_t interval = info.getConnInterval();
    linkIntervalLong = interval >= 80;
    uint32_t quietMs = now - lastBulkMs.load();
    auto request = policy.update(now, quietMs, ble_server::updateInProgress(), interval);
    if (request != BleIntervalPolicy::Request::None) {
        requestPhoneInterval(info.getConnHandle(), request == BleIntervalPolicy::Request::Relaxed,
                             request == BleIntervalPolicy::Request::Relaxed ? "idle" : "bulk traffic");
    }
    if (report || request != BleIntervalPolicy::Request::None || interval != lastInterval ||
        info.getConnLatency() != lastLatency || info.getConnTimeout() != lastTimeout || now - lastLog >= 60000) {
        diag::log("ble interval state: actual=%.1fms latency=%u timeout=%ums desired=%s bulk_idle=%lums attempts=%u phone_id=%lu",
                  interval * 1.25f, info.getConnLatency(), info.getConnTimeout() * 10,
                  policy.wantsRelaxed() ? "150-300ms" : "15-30ms", (unsigned long)quietMs,
                  policy.requestAttempts(), (unsigned long)currentEpoch);
        lastInterval = interval; lastLatency = info.getConnLatency(); lastTimeout = info.getConnTimeout();
        lastLog = now;
    }
}

}  // namespace

namespace ble_server {

// Defined further down (after the OTA/map transfer state); called from
// onDisconnect so an interrupted transfer can't wedge the device.
void otaAbortIfDownloading();
void mapAbortReceive();

// Stale-bond loop detector. When the PHONE holds pairing keys we no longer
// match, iOS auto-reconnects, fails encryption, and drops the link within
// seconds — forever, and nothing device-side can delete the phone's keys.
// What we CAN do is recognise the pattern and say the fix out loud.
uint32_t connAtMs = 0;
bool connEncrypted = false;
uint8_t quickUnencDrops = 0;

// The pairing code being shown on the panel (0 = none). Set by
// onPassKeyDisplay, cleared when authentication finishes either way; the
// timestamp bounds it so an abandoned dialog can't pin the sheet forever.
volatile uint32_t pairCode = 0;
volatile uint32_t pairCodeAtMs = 0;

void workoutAck(bool ok, const char* msg) {
    if (!workoutChr) return;
    uint8_t buf[80];
    buf[0] = 0xA0;
    buf[1] = ok ? 1 : 2;
    size_t n = strlen(msg);
    if (n > sizeof(buf) - 2) n = sizeof(buf) - 2;
    memcpy(buf + 2, msg, n);
    workoutChr->setValue(buf, n + 2);
    workoutChr->notify();
}

void sendWorkoutStatus() {
    if (!workoutChr) return;
    WorkoutView v;
    workout_service::view(v);
    uint8_t buf[64];
    buf[0] = 0xC0;
    buf[1] = v.loaded;
    buf[2] = v.running;
    buf[3] = v.paused;
    buf[4] = v.done;
    buf[5] = (uint8_t)v.segIdx;
    buf[6] = (uint8_t)v.segCount;
    memcpy(buf + 7, &v.elapsedSec, 4);
    memcpy(buf + 11, &v.totalSec, 4);
    memcpy(buf + 15, &v.targetW, 2);
    memcpy(buf + 17, &v.ftpW, 2);
    buf[19] = settings::workoutPauseEachBlock() ? 1 : 0;
    size_t n = strlen(v.name);
    if (n > sizeof(buf) - 20) n = sizeof(buf) - 20;
    memcpy(buf + 20, v.name, n);
    workoutChr->setValue(buf, 20 + n);
    workoutChr->notify();
}

void sendWorkoutList() {
    if (!workoutChr) return;
    uint8_t b = 0xB0;
    workoutChr->setValue(&b, 1);
    workoutChr->notify();
    ble_shared::listDir(ble_shared::Dir::Workouts, [](const char* base, uint32_t, void*) {
        uint8_t buf[64];
        buf[0] = 0xB1;
        size_t n = strlen(base);
        if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
        memcpy(buf + 1, base, n);
        workoutChr->setValue(buf, n + 1);
        workoutChr->notify();
        vTaskDelay(pdMS_TO_TICKS(15));   // pace notifies, same as rides
    }, nullptr);
    b = 0xB2;
    workoutChr->setValue(&b, 1);
    workoutChr->notify();
}

// Stream /workouts/<name> back to the app, MTU-sized 0xD1 chunks then 0xD2.
// The app's editor needs the actual text: blocks, ramps, percent-vs-watts.
void sendWorkoutFile(const char* name) {
    char path[96];
    snprintf(path, sizeof(path), "/workouts/%s", name);
    sdLock();
    File f = SD.open(path, FILE_READ);
    if (!f) {
        sdUnlock();
        workoutAck(false, "file not found");
        return;
    }
    // notify payload budget: MTU-3, minus our 1-byte opcode.
    size_t chunk = negotiatedMTU > 24 ? (size_t)negotiatedMTU - 4 : 20;
    if (chunk > 200) chunk = 200;
    uint8_t buf[204];
    buf[0] = 0xD1;
    size_t n;
    while ((n = f.read(buf + 1, chunk)) > 0) {
        workoutChr->setValue(buf, n + 1);
        workoutChr->notify();
        vTaskDelay(pdMS_TO_TICKS(15));   // pace notifies, same as the log send
    }
    f.close();
    sdUnlock();
    uint8_t end = 0xD2;
    workoutChr->setValue(&end, 1);
    workoutChr->notify();
}

void saveUploadedWorkout() {
    if (!workoutUpBuf || workoutUpLen == 0 || !workoutReqName[0]) {
        workoutAck(false, "empty upload");
        return;
    }
    const char* reason = "";
    const ble_shared::WorkoutSave r =
        ble_shared::saveWorkout(workoutReqName, workoutUpBuf, workoutUpLen, &reason);
    if (r == ble_shared::WorkoutSave::IoError) {
        workoutAck(false, "SD write failed");
        return;
    }
    if (r == ble_shared::WorkoutSave::Ok) workoutAck(true, workoutReqName);
    else workoutAck(false, reason);
    workoutStatusDirty = true;
}

// ---------------------------------------------------------------------------
// What the app's Ride tab shows live. The phone link (link.cpp) streams it as
// LiveFrames; this file only keeps the hand-off from the UI task, and the
// stopwatch buttons going the other way. (The unreleased LIVE byte protocol
// that used to live on b1c51001/b1c51002 is gone: those UUIDs are the link.)

// Service Changed, queued once per boot by the first admitted link.
volatile bool gattChangedDue = false;
uint32_t gattChangedDueMs = 0;              // BLE task's view, for the log lines

// What only the UI task knows (the stopwatch, the page on screen), handed
// over by publishLiveView() under a spinlock: the UI task never waits on BLE.
portMUX_TYPE liveViewMux = portMUX_INITIALIZER_UNLOCKED;
LiveView liveView;
std::atomic<uint8_t> liveViewChanges{0};   // ble_shared::LIVE_CHANGED_* bits

// Stopwatch commands from the app, a few deep so two quick taps (START then
// LAP) both land. Written by the link task, drained by the UI task
// (takeStopwatchCommand) — the spinlock is the whole hand-off; neither side
// touches the other's state.
portMUX_TYPE liveSwMux = portMUX_INITIALIZER_UNLOCKED;
constexpr uint8_t LIVE_SW_QUEUE = 4;
uint8_t liveSwQueue[LIVE_SW_QUEUE];
uint8_t liveSwHead = 0, liveSwCount = 0;

void liveSwClear() {
    portENTER_CRITICAL(&liveSwMux);
    liveSwCount = 0;   // a gone phone's taps don't run later
    portEXIT_CRITICAL(&liveSwMux);
}

void liveSwPush(uint8_t op) {
    portENTER_CRITICAL(&liveSwMux);
    if (liveSwCount < LIVE_SW_QUEUE) {   // full: a burst past 4 is dropped
        liveSwQueue[(liveSwHead + liveSwCount) % LIVE_SW_QUEUE] = op;
        ++liveSwCount;
    }
    portEXIT_CRITICAL(&liveSwMux);
}

// BLE task, every pass. iOS caches a bonded device's GATT table; this tells a
// phone paired before the link existed to look again.
void serviceGattChanged() {
    if (gattChangedDue && phoneConnected && (int32_t)(millis() - gattChangedDueMs) >= 0) {
        gattChangedDue = false;
        ble_svc_gatt_changed(0x0001, 0xFFFF);
        diag::log("ble: service changed indicated (phone rediscovers)");
    }
}

class ServerCb : public NimBLEServerCallbacks {
    uint32_t onPassKeyDisplay() override {
        // Fresh code per pairing; the panel shows it, the phone asks for it.
        uint32_t code = esp_random() % 1000000;
        // Fixed pairing: paired, nothing gets this far any more — a stranger
        // is dropped in onConnect and a known identity's pairing request is
        // refused by ble_pair_guard. Kept as the last line: never show a code
        // while paired, drop the link instead.
        if (phoneBonds.load() > 0) {
            kickLink(linkHandle, "pairing request from a second phone");
            return code;   // the link is going; nothing will show this
        }
        pairCode = code;
        pairCodeAtMs = millis();
        diag::log("ble: pairing code %06lu shown", (unsigned long)code);
        return code;
    }
    void onConnect(NimBLEServer* srv, NimBLEConnInfo& info) override {
        // The device has ONE phone slot (linkHandle and the state around it),
        // but the host allows several links and advertising can be back on
        // while a phone is connected. A second central must never overwrite
        // the slot — a stranger connecting three times a second did exactly
        // that, and each refusal's disconnect then reset the paired phone's
        // state (2026-10-10). The paired phone itself connecting again means
        // its old link is dead from its side: the new link takes the slot.
        bool known = NimBLEDevice::isBonded(info.getIdAddress()) ||
                     NimBLEDevice::isBonded(info.getAddress());
        if (linkUp && info.getConnHandle() != linkHandle) {
            if (!(known && phoneBonds.load() > 0)) {
                diag::log("ble: extra link from %s while the phone slot is busy "
                          "— refused", info.getIdAddress().toString().c_str());
                kickLink(info.getConnHandle(), "phone slot busy", false);
                return;
            }
            diag::log("ble: the paired phone reconnected over a stale link — "
                      "dropping the old one");
            uint16_t old = linkHandle;
            linkHandle = info.getConnHandle();   // old link's disconnect is ignored
            ams::onDisconnect(old);
            srv->disconnect(old);
        }
        negotiatedMTU = info.getMTU();
        linkUp = true;
        linkHandle = info.getConnHandle();
        linkAdmitted = false;
        linkRefused = false;
        // getIdAddress() is the bonded identity when NimBLE resolved the
        // phone's private address against a stored IRK — the same resolution
        // AMS reconnects have always relied on.
        linkPeerKnown = known;
        connAtMs = millis();
        connEncrypted = false;
        static uint32_t lastUpLogMs = 0;
        if (linkPeerKnown || phoneBonds.load() == 0 || lastUpLogMs == 0 ||
            millis() - lastUpLogMs > 30000) {
            lastUpLogMs = millis();
            diag::log("phone link up: %s (id %s) MTU=%u interval=%.1fms paired=%d known_peer=%d",
                      info.getAddress().toString().c_str(),
                      info.getIdAddress().toString().c_str(),
                      info.getMTU(), info.getConnInterval() * 1.25f,
                      phoneBonds.load() > 0, (int)linkPeerKnown);
        }
        // Paired, and this isn't the paired phone: gone now, before the
        // security request below — that request is what makes a phone without
        // our keys start pairing and put a passkey dialog up. No interval
        // request, no bulk window either: nothing on this link is a phone yet.
        if (phoneBonds.load() > 0 && !linkPeerKnown) {
            kickLink(info.getConnHandle(), "not the paired phone");
            return;
        }
        // A pairing request from the bonded identity is a phone that forgot
        // us: refused in refuseRepeatPairing, before the host answers it.
        int rc = ble_pair_guard_install(info.getConnHandle());
        if (rc != 0) diag::log("ble: pairing guard not installed (rc=%d)", rc);
        // Start the AMS handshake (encrypt -> discover the phone's own media
        // service). Runs in the server task, not here. Its security request is
        // also what makes a bonded phone encrypt, and so be admitted, even
        // when no app is open to read anything — and, unpaired, what starts
        // the pairing that puts the code on the panel.
        ams::onConnect(info.getConnHandle());
    }
    // The link proved it holds our bond: it is THE phone. Only now does it
    // get treated as one — the fast interval for the app's initial sync,
    // pushes, the status bar icon.
    void admit(NimBLEConnInfo& info, bool newPairing) {
        // A key refresh on an admitted link raises another authentication-
        // complete event; the link (and its phone-link session) carries on.
        if (linkAdmitted && phoneConnected) return;
        linkAdmitted = true;
        phoneConnected = true;
        ++phoneConnectionEpoch;
        phone_link::onAdmitted();   // a fresh link session (no Hello yet)
        noteBulk();
        requestPhoneInterval(info.getConnHandle(), false, "admitted");
        settings::setPhoneId(info.getIdAddress().toString().c_str());
        if (newPairing) {   // a new owner: the old name is not theirs
            phoneNameCache[0] = 0;
            settings::setPhoneName("");
        }
        recountPhoneBonds();
        if (newPairing)
            diag::log("ble: paired with phone %s — only this phone can connect "
                      "now (unpair: Paired Devices > Phone)",
                      info.getIdAddress().toString().c_str());
        diag::log("phone connected: admitted (%s)", newPairing ? "new pairing" : "bonded");
    }
    // Drop a link that finished security without earning admission. A bond
    // this link just created is undone; the paired phone's bond never is.
    void reject(NimBLEConnInfo& info, const char* why) {
        pairCode = 0;
        if (!linkPeerKnown && info.isBonded()) {
            NimBLEDevice::deleteBond(info.getIdAddress());
            recountPhoneBonds();
        }
        if (phoneBonds.load() > 0) {
            kickLink(info.getConnHandle(), why);
        } else {
            diag::log("ble: dropping link (%s) — pair again with the code on "
                      "the panel", why);
            NimBLEDevice::getServer()->disconnect(info.getConnHandle(),
                                                  BLE_ERR_AUTH_FAIL);
        }
    }
    void onAuthenticationComplete(NimBLEConnInfo& info) override {
        diag::log("ble: link %s (bonded=%d authenticated=%d key=%u)",
                  info.isEncrypted() ? "encrypted" : "NOT encrypted",
                  info.isBonded(), info.isAuthenticated(), info.getSecKeySize());
        if (info.getConnHandle() != linkHandle) return;
        if (!info.isEncrypted()) {
            // Paired: the bonded identity failed to encrypt — stale or
            // mismatched keys, or not the phone at all. Its bond stays (see
            // "Fixed pairing"); the link goes.
            if (phoneBonds.load() > 0) {
                pairCode = 0;
                kickLink(info.getConnHandle(), "encryption failed — keys don't "
                         "match the paired phone");
                return;
            }
            // Unpaired: a cancelled or mistyped passkey. Keep the link — the
            // rider may try again from the phone, and the 90 s unproven-link
            // timeout in task() reclaims it otherwise.
            pairCode = 0;
            ams::onSecured(info.getConnHandle(), false);
            return;
        }
        const bool wasPaired = phoneBonds.load() > 0;
        if (!info.isBonded()) {
            // Encrypted but nothing stored (the phone declined bonding):
            // there is nothing to recognise it by next time, so it can't
            // own the device.
            reject(info, "encrypted without bonding");
            return;
        }
        if (!info.isAuthenticated() || info.getSecKeySize() < 16) {
            // Just Works (a peer with no keyboard), or a short key: nobody
            // checked a code, so this could be any phone in radio range.
            reject(info, "pairing was not authenticated with the passkey");
            return;
        }
        if (wasPaired && !linkPeerKnown) {
            // A new bond made while paired — nothing above should let one
            // through, but if it happens, undo it.
            reject(info, "new bond while paired");
            return;
        }
        connEncrypted = true;
        quickUnencDrops = 0;   // a healthy pairing ends the streak
        pairCode = 0;          // done — drop the code sheet
        admit(info, !linkPeerKnown);
        ams::onSecured(info.getConnHandle(), true);
        // iOS caches a bonded device's GATT table and reuses it until told
        // otherwise, so services a firmware update added (the live stream)
        // stayed invisible to a phone paired before it. Once per boot — every
        // update ends in one — tell the phone the whole table may have
        // changed; it rediscovers (cheap) and finds what's there now.
        // Sent from the BLE task a moment later, not here: this callback runs
        // before NimBLE restores the bonded phone's subscriptions for the
        // link, so an indication from here found nobody subscribed.
        static bool gattChangedQueued = false;
        if (!gattChangedQueued) {
            gattChangedQueued = true;
            gattChangedDueMs = millis() + 2000;
            gattChangedDue = true;
        }
    }
    void onConnParamsUpdate(NimBLEConnInfo& info) override {
        diag::log("ble interval negotiated: handle=%u interval=%.1fms latency=%u timeout=%ums",
                  info.getConnHandle(), info.getConnInterval() * 1.25f,
                  info.getConnLatency(), info.getConnTimeout() * 10);
    }
    void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override {
        diag::log("MTU negotiated: %u", mtu);
        negotiatedMTU = mtu;
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) override {
        // Not the phone slot's link (a refused extra, or the stale link a
        // reconnecting phone replaced): nothing of the phone's to reset, and
        // no advertising while the phone is connected.
        if (info.getConnHandle() != linkHandle) {
            if (!linkUp && !unpairPending) {
                if (advDirty) applyAdvertising();
                else NimBLEDevice::startAdvertising();
            }
            return;
        }
        // A refused stranger reconnects every few hundred ms; its drops are
        // already counted in kickLink's rate-limited line.
        if (!linkRefused) diag::log("phone disconnected (reason %d)", reason);
        // A supervision timeout (520) while the CPU was allowed to sleep on the
        // relaxed link is the experiment's failure signature — count it, and
        // pull the plug after three. Other reasons (531 = the app closed the
        // link on purpose) say nothing about sleep and don't count.
        bool sleepEligibleInterval = linkIntervalLong;
#ifdef PM_BLE_XTAL
        sleepEligibleInterval = true;
#endif
        uint32_t slept, rejected; uint64_t sleepUs;
        power_mgmt::sleepStats(slept, rejected, sleepUs);
        if (!linkRefused)
            diag::log("ble disconnect: interval_long=%d sleep_allowed=%d successful_sleep_calls=%lu",
                      linkIntervalLong, relaxedSleepOk, (unsigned long)slept);
        if (reason == 520 && sleepEligibleInterval && relaxedSleepOk) {
            if (++relaxedDropCount >= 3) {
                relaxedSleepOk = false;
                diag::log("ble: light sleep with phone DISABLED "
                          "(3 supervision timeouts while connected sleep allowed)");
            }
        }
        const bool refused = linkRefused;
        linkIntervalLong = false;
        phoneConnected = false;
        linkUp = false;
        linkAdmitted = false;
        linkPeerKnown = false;
        linkRefused = false;
        linkHandle = 0xFFFF;
        pairCode = 0;             // a refused or abandoned pairing's sheet goes too
        mediaTrackFeed = false;   // the next app must ask again
        liveSwClear();
        phone_link::onDisconnected();   // link state is per connection
        if (refused) {
            // Refused while paired: expected, and already logged.
        } else if (!connEncrypted && millis() - connAtMs < 15000) {
            if (++quickUnencDrops == 3)
                diag::log("ble: 3 quick unencrypted drops — the phone likely "
                          "holds stale pairing keys. Fix: iPhone Settings > "
                          "Bluetooth > OpenTrailPaper > Forget This Device, then "
                          "reconnect and pair fresh.");
        } else if (connEncrypted) {
            quickUnencDrops = 0;
        }
        ams::onDisconnect(info.getConnHandle());
        // Recover from a transfer interrupted by the disconnect, so the device
        // doesn't stay frozen on the update popup / refuse the next attempt.
        otaAbortIfDownloading();
        mapAbortReceive();
        if (sensorsStreaming) {   // app's sensor screen can't still be open
            sensorsStreaming = false;
            ble_sensors::setScanAlways(false);
        }
        // Advertising stops once a central connects; restart it so the phone
        // can find and reconnect to the device after disconnecting — with a
        // fresh advertisement if the pairing state changed on this link.
        if (unpairPending) return;   // serviceUnpair re-advertises when done
        if (advDirty) applyAdvertising();
        else NimBLEDevice::startAdvertising();
    }
};

void otaNotify(const uint8_t* d, size_t n) {
    if (!otaChr) return;
    otaChr->setValue(d, n);
    otaChr->notify();
}
void otaNotify1(uint8_t b) { otaNotify(&b, 1); }

// Over-the-air firmware update. CRITICAL: flash erase/write must NOT run in
// the BLE callback — erasing the 1.5 MB OTA partition blocks the NimBLE host
// task long enough to drop the connection. So the callback only buffers the
// incoming firmware into PSRAM (fast memcpy); all flashing happens later in
// the server task, off the BLE host. A/B partitions mean the running image is
// never touched, so a failed/interrupted transfer can't brick the device.
// Protocol on CHR_OTA (write + notify):
//   phone -> [0x01][u32 size][opt 32-byte md5 hex]  begin
//           [0x02]<firmware bytes>                   data (sent .withResponse)
//           [0x03]                                   end / commit
//           [0x04]                                   abort
//           [0x05]                                   query running version
//   device notifies: [0xA0] ready, [0xA1] success(rebooting), [0xA2] aborted,
//           [0xA3]<utf8 version>, [0xA4][u32 received] progress, [0xAF][err]
uint8_t* otaBuf = nullptr;                 // PSRAM staging buffer
volatile uint32_t otaBufLen = 0, otaBufCap = 0;
volatile int otaPhase = 0;   // 0 idle, 1 downloading, 2 installing (for the UI)
char otaMd5[33] = "";
volatile bool otaCommitPending = false;    // task should flash the buffer
uint32_t otaStartMs = 0, otaCommitStartMs = 0;
volatile uint32_t otaLastDataMs = 0;       // for the stalled-download timeout
// The phone link's firmware upload, for the popup (ble_shared::setLinkUpdate).
volatile int linkUpdPhase = 0;
volatile uint32_t linkUpdGot = 0, linkUpdTotal = 0;

void otaFreeBuf() {
    if (otaBuf) { heap_caps_free(otaBuf); otaBuf = nullptr; }
    otaBufLen = otaBufCap = 0;
}

// Abort a download-phase OTA (disconnect or stalled). NEVER touches an
// in-flight install (otaPhase 2) — that owns otaBuf and will reboot.
void otaAbortIfDownloading() {
    if (otaPhase == 1) {
        otaPhase = 0;
        otaFreeBuf();
        diag::log("ota download aborted (recovered)");
    }
}

class OtaCb : public NimBLECharacteristicCallbacks {
    uint32_t lastProgress = 0;
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        noteBulk();   // a 2 MB firmware push at 300 ms/packet would take hours
        std::string v = c->getValue();
        if (v.empty()) return;
        const uint8_t* p = (const uint8_t*)v.data();
        size_t n = v.size();
        switch (p[0]) {
        case 0x05: {                          // running firmware version
            uint8_t buf[24];
            buf[0] = 0xA3;
            size_t vl = strlen(FIRMWARE_VERSION);
            if (vl > sizeof(buf) - 1) vl = sizeof(buf) - 1;
            memcpy(buf + 1, FIRMWARE_VERSION, vl);
            otaNotify(buf, 1 + vl);
            break;
        }
        case 0x01: {                          // begin: allocate PSRAM buffer
            if (n < 5) return;
            uint32_t expected = (uint32_t)p[1] | ((uint32_t)p[2] << 8) |
                                ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
            otaFreeBuf();
            // A firmware image wants ~2 MB of PSRAM in ONE piece, and the tile
            // cache is exactly the long-lived block that fragments that away.
            // The map just re-reads from the card afterwards.
            map_store::releaseCache();
            otaBuf = (uint8_t*)heap_caps_malloc(expected, MALLOC_CAP_SPIRAM);
            if (!otaBuf) {
                uint8_t e[2] = {0xAF, 0xF0};   // out of memory
                otaNotify(e, 2);
                return;
            }
            otaBufCap = expected;
            otaBufLen = 0;
            lastProgress = 0;
            otaMd5[0] = 0;
            if (n >= 5 + 32) { memcpy(otaMd5, p + 5, 32); otaMd5[32] = 0; }
            otaPhase = 1;                      // downloading -> device popup
            otaStartMs = millis();
            otaLastDataMs = millis();
            otaNotify1(0xA0);
            diag::log("ota begin: %u bytes, MTU=%u", (unsigned)expected, negotiatedMTU);
            break;
        }
        case 0x02: {                          // data: memcpy into PSRAM (no flash)
            if (otaPhase != 1 || !otaBuf) return;
            otaLastDataMs = millis();
            size_t got = n - 1;
            if (otaBufLen + got > otaBufCap) got = otaBufCap - otaBufLen;
            memcpy(otaBuf + otaBufLen, p + 1, got);
            otaBufLen += got;
            if (otaBufLen - lastProgress >= 32768) {
                lastProgress = otaBufLen;
                uint8_t pr[5] = {0xA4};
                uint32_t r = otaBufLen;
                memcpy(pr + 1, &r, 4);
                otaNotify(pr, 5);
            }
            break;
        }
        case 0x03: {                          // end: hand off to the task to flash
            if (otaPhase != 1) return;
            otaCommitPending = true;
            uint32_t dt = millis() - otaStartMs;
            float kbps = dt ? (otaBufLen / 1024.0f) / (dt / 1000.0f) : 0;
            diag::log("ota transfer done: %u bytes in %.1fs (%.1f KB/s)",
                      (unsigned)otaBufLen, dt / 1000.0f, kbps);
            break;
        }
        case 0x04:                            // abort
            otaPhase = 0;
            otaFreeBuf();
            otaNotify1(0xA2);
            break;
        }
    }
};

// Vector-map transfer (phone -> device). Structurally identical to OTA: the
// callback only stages the .ebm into PSRAM; the SD write + activate happens in
// the server task (SD writes must not block the BLE host). Protocol on
// CHR_MAP (write + write-no-response + notify):
//   phone -> [0x01][u32 size][name…]  begin whole map
//           [0x06][u32 size][h3id…]   begin one H3 tile (-> /maps/tiles)
//           [0x02]<bytes>  data        [0x03] end     [0x04] abort
//           [0x05]  list whole-map coverage    [0x07]  list H3 tile ids
//           [0x08]  list H3 ids that have a cycling-POI file (<id>.poi)
//           [0x09]  get map layers   [0x0A][u16 mask]  set map layers
//   map-layers reply: [0xE0][u16 mask LE] — to 0x09, to 0x0A (the mask now in
//   force), and unasked after an edit on the device. Bits: MAP_LAYER_* in
//   map_view.h (0 bike routes, 1 cycleways & lanes, 2 water, 3 toilets,
//   4 repair, 5 bike shops). Firmware without layers never answers, which is
//   how the app knows to hide its switches. Kept OUT of the settings payload
//   on purpose: this is self-describing and needs no payload version.
//   device notifies: [0xB0] ready, [0xB1] saved, [0xB4][u32 received], [0xBF][err]
//   tile-list reply: [0xD0] begin, [0xD1]<h3id chars> per tile, [0xD2] end
//   poi-list reply:  [0xD3] begin, [0xD4]<h3id chars> per tile, [0xD5] end
//   A .poi travels as a tile ([0x06], same id): saveTile tells it apart by its
//   'EPOI' magic. Firmware without POI support never answers 0x08, which is how
//   the app knows not to send it .poi files.
//   Tile versions (docs/prebuilt-tiles.md#versions, tile_versions.h): the
//   device keeps the version record each tile and .poi was sent with, so every
//   phone can tell a current hex from an update — not only the one that sent it.
//     [0x07][0x01] / [0x08][0x01]  list, with versions. Answered with a flagged
//       begin, [0xD0][0x01] / [0xD3][0x01], and entries "<h3id>" or
//       "<h3id>=<record>" in the same comma-separated 0xD1 / 0xD4 packets.
//       Older firmware ignores the flag byte and answers the plain 1-byte
//       begin, which is how the app knows it keeps no versions.
//     [0x06][u32 size][h3id][0x00][record]  begin a tile with its version.
//       The app sends the record only to firmware that answered a flagged
//       begin (older firmware would stop at the NUL anyway).
NimBLECharacteristic* mapChr = nullptr;
uint8_t* mapBuf = nullptr;
volatile uint32_t mapBufLen = 0, mapBufCap = 0;
char mapName[52] = "";
char mapVersion[tile_versions::VER_MAX] = "";   // staged tile's record, "" = none
volatile bool mapCommitPending = false;
volatile bool mapIsTile = false;         // staged buffer is an H3 tile, not a whole map
volatile bool mapListReq = false;
volatile bool tileListReq = false;
volatile bool poiListReq = false;
volatile bool tileListVersions = false;  // the pending list asked for versions
volatile bool poiListVersions = false;
volatile bool mapLayersNotify = false;   // send [0xE0][mask] from the server task
// A device-side layer edit: pushed on the link when this connection said
// Hello, else as the [0xE0] notify above.
volatile bool mapLayersPushDue = false;
uint32_t mapStartMs = 0, mapLastProgress = 0;

void mapNotify(const uint8_t* d, size_t n) {
    if (!mapChr) return;
    mapChr->setValue(d, n);
    mapChr->notify();
}
void mapFreeBuf() {
    if (mapBuf) { heap_caps_free(mapBuf); mapBuf = nullptr; }
    mapBufLen = mapBufCap = 0;
}

// Drop a partially-received map on disconnect. Leaves a queued commit alone.
void mapAbortReceive() {
    if (!mapCommitPending) mapFreeBuf();
}

class MapCb : public NimBLECharacteristicCallbacks {
    bool receiving = false;
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        noteBulk();   // map tile upload chunks
        std::string v = c->getValue();
        if (v.empty()) return;
        const uint8_t* p = (const uint8_t*)v.data();
        size_t n = v.size();
        switch (p[0]) {
        case 0x01:                                // begin whole map
        case 0x06: {                              // begin one H3 tile
            if (n < 5) return;
            uint32_t expected = (uint32_t)p[1] | ((uint32_t)p[2] << 8) |
                                ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
            mapFreeBuf();
            mapBuf = (uint8_t*)heap_caps_malloc(expected, MALLOC_CAP_SPIRAM);
            if (!mapBuf) { uint8_t e[2] = {0xBF, 0xF0}; mapNotify(e, 2); return; }
            mapBufCap = expected;
            mapBufLen = 0;
            mapLastProgress = 0;
            mapIsTile = (p[0] == 0x06);
            // [name][0x00][version record] — the record only from apps that
            // saw this firmware list versions.
            const uint8_t* nul = (const uint8_t*)memchr(p + 5, 0, n - 5);
            size_t nameLen = nul ? (size_t)(nul - (p + 5)) : n - 5;
            size_t nl = nameLen < sizeof(mapName) - 1 ? nameLen : sizeof(mapName) - 1;
            memcpy(mapName, p + 5, nl);
            mapName[nl] = 0;
            mapVersion[0] = 0;
            if (nul && mapIsTile) {
                size_t vl = n - 5 - nameLen - 1;
                if (vl < sizeof(mapVersion)) {   // longer: not a record we know
                    memcpy(mapVersion, nul + 1, vl);
                    mapVersion[vl] = 0;
                }
            }
            if (!mapName[0])
                snprintf(mapName, sizeof(mapName), mapIsTile ? "tile" : "map.ebm");
            receiving = true;
            mapStartMs = millis();
            mapNotify1_B0();
            diag::log("%s recv begin: %u bytes '%s'", mapIsTile ? "tile" : "map",
                      (unsigned)expected, mapName);
            break;
        }
        case 0x02: {                              // data
            if (!receiving || !mapBuf) return;
            size_t got = n - 1;
            if (mapBufLen + got > mapBufCap) got = mapBufCap - mapBufLen;
            memcpy(mapBuf + mapBufLen, p + 1, got);
            mapBufLen += got;
            if (mapBufLen - mapLastProgress >= 32768) {
                mapLastProgress = mapBufLen;
                uint8_t pr[5] = {0xB4};
                uint32_t r = mapBufLen;
                memcpy(pr + 1, &r, 4);
                mapNotify(pr, 5);
            }
            break;
        }
        case 0x03:                                // end -> commit in task
            if (!receiving) return;
            receiving = false;
            mapCommitPending = true;
            break;
        case 0x04:                                // abort
            receiving = false;
            mapFreeBuf();
            break;
        case 0x05:                                // list device map coverage
            mapListReq = true;
            break;
        case 0x07:                                // list H3 tile ids on SD
            tileListVersions = n >= 2 && (p[1] & 0x01);
            tileListReq = true;
            break;
        case 0x08:                                // list H3 ids with a .poi
            poiListVersions = n >= 2 && (p[1] & 0x01);
            poiListReq = true;
            break;
        case 0x09:                                // get map layers
            mapLayersNotify = true;
            break;
        case 0x0A:                                // set map layers
            if (n >= 3) {
                settings::setMapLayers((uint16_t)(p[1] | (p[2] << 8)));
                mapLayersNotify = true;           // echo what is now in force
            }
            break;
        }
    }
    void mapNotify1_B0() { uint8_t b = 0xB0; mapNotify(&b, 1); }
};

// AGNSS ephemeris injection: the phone streams a module-specific ephemeris blob
// and we pipe the raw bytes to the receiver's UART (gps_service parses nothing —
// the module ingests its own format). See investigations/agnss.md.
NimBLECharacteristic* agnssChr = nullptr;
void agnssNotify(const uint8_t* d, size_t n) {
    if (agnssChr) { agnssChr->setValue(d, n); agnssChr->notify(); }
}
class AgnssCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        std::string v = c->getValue();
        if (v.empty()) return;
        const uint8_t* p = (const uint8_t*)v.data();
        size_t n = v.size();
        switch (p[0]) {
        case 0x10: {                              // query module + fix state
            RideState s = g_state.snapshot();
            uint8_t r[3] = {0x10, (uint8_t)gps_service::moduleKindCode(),
                            (uint8_t)(s.gpsFix ? 1 : 0)};
            agnssNotify(r, 3);
            break;
        }
        case 0x01:                                // begin
            gps_service::agnssBegin();
            { uint8_t a = 0xA1; agnssNotify(&a, 1); }
            break;
        case 0x02:                                // ephemeris bytes
            if (n > 1) gps_service::agnssInject(p + 1, n - 1);
            break;
        case 0x03:                                // end
            gps_service::agnssEnd();
            { uint8_t d = 0xA3; agnssNotify(&d, 1); }
            break;
        case 0x04:                                // abort
            gps_service::agnssBegin();             // reset the stream, drop bytes
            break;
        }
    }
};

// ---------------------------------------------------------------------------
// Meshtastic messaging bridge (phone <-> device <-> LoRa mesh)
// ---------------------------------------------------------------------------
//
// The device is the radio; the app is the keyboard and the screen. Everything
// here is small and text-shaped, so unlike the ride/map transfers it needs no
// windowing — one notification per message, each inside the 247-byte MTU.
//
//   Phone -> device:
//     [0x01]                                   send me the state
//     [0x02][u32 dest][utf8 text]              send a message (dest ffffffff =
//                                              broadcast to the channel)
//     [0x03]                                   send me the message history
//     [0x04]                                   send me the node list
//     [0x05][u8 longLen][long…][short…]        rename this node
//     [0x06][u8 on]                            radio on / off
//     [0x07][u8 keyIndex][utf8 channel name]   switch channel
//     [0x08]                                   mark everything read
//     [0x09]                                   send me the packet counters
//     [0x0a]                                   send me the modem preset list
//     [0x0b][u8 index]                         set the modem preset
//     [0x0c]                                   send me the channel list
//     [0x0d][u8 idx][u8 nameLen][name][psk]    add / replace a private channel
//     [0x0e][u8 idx]                           forget a private channel
//     [0x0f][u8 idx][u8 on]                    share our position on a channel
//     [0x10]                                   send me the region list
//     [0x11][u8 index]                         set the region
//     [0x12][i8 dBm]                           set TX power (0 = the maximum
//                                              the region and radio allow)
//     [0x13][u8 slot]                          pin the frequency slot, 1-based
//                                              (0 = derive it from the name)
//
//   Device -> phone (notify):
//     [0x90] state    flags, node number, frequency, channel, our names, then
//                     preset index and (appended later, optional for an older
//                     app) region index, TX dBm, slot override, slot count,
//                     active slot
//     [0x91] message  one per message, oldest first
//     [0x92]          end of history
//     [0x93] node     one per known neighbour, with its position if it has sent one
//     [0x94]          end of node list
//     [0x95] stats    packet counters
//     [0x96]          something changed — ask again
//     [0x97][u32 id]  a message was queued, with the id its acks will quote
//     [0x98]          the device refused to queue it (radio off, or outbox full)
//     [0x99] preset   one modem preset the device supports
//     [0x9a]          end of the preset list
//     [0x9b] channel  one channel, WITH its key — the phone needs it to build a
//                     share QR, and this link is already the trusted one
//     [0x9c]          end of the channel list
//     [0x9d] region   one region the device offers: index, band, power limit, name
//     [0x9e]          end of the region list
NimBLECharacteristic* meshChr = nullptr;

// Requests staged by the write callback, serviced in the task. A bitmask rather
// than an enum because the app asks for several of these at once when its
// Messages screen opens, and one flag per request cannot lose the others.
constexpr uint8_t MREQ_STATE   = 0x01;
constexpr uint8_t MREQ_HISTORY = 0x02;
constexpr uint8_t MREQ_NODES   = 0x04;
constexpr uint8_t MREQ_STATS   = 0x08;
constexpr uint8_t MREQ_PRESETS = 0x10;
constexpr uint8_t MREQ_CHANNELS = 0x20;
constexpr uint8_t MREQ_REGIONS = 0x40;
volatile uint8_t meshReq = 0;

// Outgoing text, staged the same way. queueText() is cheap enough to call from a
// BLE callback, but the reply (the packet id, or a refusal) has to be notified,
// and notify() from inside onWrite is what the rest of this file exists to avoid.
constexpr int MESH_SEND_QUEUE = 4;
struct StagedText {
    uint32_t dest;
    uint8_t channel;
    char text[mesh::MAX_TEXT_LEN + 1];
};
StagedText meshSendQ[MESH_SEND_QUEUE];
volatile int meshSendCount = 0;

// Staged config edits.
char meshSetLong[40], meshSetShort[8];
volatile bool meshNamesPending = false;
char meshSetChan[16];
volatile uint8_t meshSetKey = 1;
volatile bool meshChanPending = false;
volatile int8_t meshEnablePending = -1;   // -1 nothing, 0 off, 1 on
volatile int8_t meshPresetPending = -1;   // -1 nothing, else a preset index
volatile int8_t meshRegionPending = -1;   // -1 nothing, else a region index
volatile int16_t meshTxPending = -1;      // -1 nothing, else dBm (0 = max)
volatile int16_t meshSlotPending = -1;    // -1 nothing, else 0 (auto) or a slot

// Staged private-channel edits. pskLen 0xFF is the delete marker.
volatile int8_t meshChanIdxPending = -1;
char meshChanNamePending[16];
uint8_t meshChanPskPending[mesh::MAX_PSK_LEN];
volatile uint8_t meshChanPskLenPending = 0;

void meshNotify(const uint8_t* d, size_t n) {
    if (!meshChr) return;
    meshChr->setValue(d, n);
    meshChr->notify();
}

// Appends a length-prefixed UTF-8 string. Used for every name in this protocol
// so the app never has to guess where one field ends.
size_t putStr(uint8_t* p, const char* s, size_t maxLen) {
    size_t n = strnlen(s ? s : "", maxLen);
    p[0] = (uint8_t)n;
    memcpy(p + 1, s, n);
    return 1 + n;
}

void sendMeshState() {
    uint8_t pkt[96];
    int p = 0;
    pkt[p++] = 0x90;
    pkt[p++] = (uint8_t)((mesh_service::enabled() ? 1 : 0) |
                         (mesh_service::radioOk() ? 2 : 0) |
                         // bit 2: the channel name came from the modem preset
                         // rather than being pinned, which is the interoperable
                         // default and worth the app being able to say so.
                         (mesh_service::channelFollowsPreset() ? 4 : 0));
    uint32_t num = mesh_service::nodeNum();
    memcpy(pkt + p, &num, 4); p += 4;
    // Hz rather than MHz so the app has no float to parse — 906875000 fits a u32.
    uint32_t hz = (uint32_t)(mesh_service::frequencyMHz() * 1e6f + 0.5f);
    memcpy(pkt + p, &hz, 4); p += 4;
    pkt[p++] = mesh_service::channelPskIndex();
    pkt[p++] = (uint8_t)mesh_service::nodeCount();   // capped at MESH_NODE_MAX
    // Clamped, not truncated: 256 unread would otherwise arrive as 0 and clear
    // the app's badge at exactly the moment it matters most.
    const int un = mesh_service::unreadCount();
    pkt[p++] = (uint8_t)(un > 255 ? 255 : un);
    p += putStr(pkt + p, mesh_service::channelName(), 15);
    p += putStr(pkt + p, mesh_service::shortName(), 7);
    p += putStr(pkt + p, mesh_service::longName(), 39);
    // Appended last so the layout above stays stable: the modem is a second,
    // independent axis from the channel and the app shows both.
    pkt[p++] = mesh_service::presetIndex();
    // And after it, the radio configuration an app that predates it simply
    // does not read. Slot numbers are 1-based here as in Meshtastic's UI.
    pkt[p++] = mesh_service::regionIndex();
    pkt[p++] = (uint8_t)mesh_service::txPowerDbm();
    pkt[p++] = mesh_service::freqSlot();
    const uint32_t nslots = mesh_service::slotCount();
    pkt[p++] = (uint8_t)(nslots > 255 ? 255 : nslots);
    const uint32_t aslot = mesh_service::activeSlot();
    pkt[p++] = (uint8_t)(aslot > 255 ? 255 : aslot);
    meshNotify(pkt, p);
}

void sendMeshRegions() {
    for (int i = 0; i < mesh::REGION_COUNT; ++i) {
        const mesh::Region& r = mesh::kRegions[i];
        uint8_t pkt[40];
        int p = 0;
        pkt[p++] = 0x9d;
        pkt[p++] = (uint8_t)i;
        // Hz, like the state packet's frequency: integers, nothing to parse.
        const uint32_t s = (uint32_t)(r.startMHz * 1e6f + 0.5f);
        const uint32_t e = (uint32_t)(r.endMHz * 1e6f + 0.5f);
        const uint32_t g = (uint32_t)(r.spacingMHz * 1e6f + 0.5f);
        memcpy(pkt + p, &s, 4); p += 4;
        memcpy(pkt + p, &e, 4); p += 4;
        memcpy(pkt + p, &g, 4); p += 4;
        pkt[p++] = (uint8_t)r.powerLimitDbm;
        p += (int)putStr(pkt + p, r.name, 15);
        sendChunk(meshChr, pkt, p);
    }
    uint8_t end = 0x9e;
    sendChunk(meshChr, &end, 1);
}

void sendMeshHistory() {
    const int n = mesh_service::messageCount();
    for (int i = 0; i < n; ++i) {
        mesh_service::Message m;
        if (!mesh_service::messageAt(i, m)) continue;
        uint8_t pkt[240];
        int p = 0;
        pkt[p++] = 0x91;
        memcpy(pkt + p, &m.id, 4); p += 4;
        memcpy(pkt + p, &m.from, 4); p += 4;
        memcpy(pkt + p, &m.to, 4); p += 4;
        memcpy(pkt + p, &m.utc, 4); p += 4;
        // Age, not a timestamp: until GPS has set the clock the device has no
        // idea what time it is, and the phone always does.
        uint32_t age = millis() - m.ms;
        memcpy(pkt + p, &age, 4); p += 4;
        pkt[p++] = (uint8_t)m.rssi;
        pkt[p++] = (uint8_t)m.snr;
        pkt[p++] = m.hops;
        pkt[p++] = (uint8_t)((m.outgoing ? 1 : 0) | (m.status << 4));
        size_t tn = strnlen(m.text, mesh::MAX_TEXT_LEN);
        pkt[p++] = (uint8_t)tn;
        memcpy(pkt + p, m.text, tn); p += (int)tn;
        // Appended after the text so the layout above is untouched: which channel
        // a message belongs to is what separates a private thread from the public
        // one, so the app cannot group them without it.
        pkt[p++] = m.channel;
        sendChunk(meshChr, pkt, p);
    }
    uint8_t end = 0x92;
    sendChunk(meshChr, &end, 1);
}

void sendMeshNodes() {
    const int n = mesh_service::nodeCount();
    for (int i = 0; i < n; ++i) {
        mesh_service::Node nd;
        if (!mesh_service::nodeAt(i, nd)) continue;
        // 12 fixed + 8 short + 40 long + 17 position = 77 worst case.
        uint8_t pkt[112];
        int p = 0;
        pkt[p++] = 0x93;
        memcpy(pkt + p, &nd.num, 4); p += 4;
        uint32_t age = millis() - nd.lastHeardMs;
        memcpy(pkt + p, &age, 4); p += 4;
        pkt[p++] = (uint8_t)nd.snr;
        pkt[p++] = (uint8_t)nd.rssi;
        pkt[p++] = nd.hops;
        p += (int)putStr(pkt + p, nd.shortName, 7);
        p += (int)putStr(pkt + p, nd.longName, 39);
        // Position, appended so the layout above stays put. A node that has never
        // broadcast one sends the flag and nothing else: "has not told us" and
        // "is at 0,0" have to stay distinguishable.
        pkt[p++] = nd.hasPosition ? 1 : 0;
        if (nd.hasPosition) {
            const int32_t latE7 = (int32_t)(nd.latitude * 1e7);
            const int32_t lonE7 = (int32_t)(nd.longitude * 1e7);
            memcpy(pkt + p, &latE7, 4); p += 4;
            memcpy(pkt + p, &lonE7, 4); p += 4;
            const int16_t alt = (int16_t)(nd.altitudeM < -32000 ? -32000
                                          : nd.altitudeM > 32000 ? 32000
                                                                 : nd.altitudeM);
            memcpy(pkt + p, &alt, 2); p += 2;
            pkt[p++] = nd.satsInView;
            pkt[p++] = nd.precisionBits;
            const uint32_t posAge = millis() - nd.positionMs;
            memcpy(pkt + p, &posAge, 4); p += 4;
        }
        sendChunk(meshChr, pkt, p);
    }
    uint8_t end = 0x94;
    sendChunk(meshChr, &end, 1);
}

void sendMeshChannels() {
    for (int i = 0; i < mesh::MAX_CHANNELS; ++i) {
        mesh_service::ChannelInfo ci;
        if (!mesh_service::channelAt(i, ci)) continue;
        uint8_t pkt[64];
        int p = 0;
        pkt[p++] = 0x9b;
        pkt[p++] = ci.index;
        pkt[p++] = ci.hash;
        p += (int)putStr(pkt + p, ci.name, 15);
        // The key goes to the phone because building a share QR needs it. The BLE
        // link is the one the rider already trusts with everything else here.
        pkt[p++] = ci.pskLen;
        memcpy(pkt + p, ci.psk, ci.pskLen); p += ci.pskLen;
        pkt[p++] = ci.sharesLocation ? 1 : 0;
        sendChunk(meshChr, pkt, p);
    }
    uint8_t end = 0x9c;
    sendChunk(meshChr, &end, 1);
}

void sendMeshPresets() {
    for (int i = 0; i < mesh::PRESET_COUNT; ++i) {
        const mesh::ModemPreset& mp = mesh::kPresets[i];
        uint8_t pkt[32];
        int p = 0;
        pkt[p++] = 0x99;
        pkt[p++] = (uint8_t)i;
        pkt[p++] = mp.sf;
        const uint16_t bw10 = (uint16_t)(mp.bwKhz * 10.0f + 0.5f);   // 250.0 -> 2500
        pkt[p++] = (uint8_t)(bw10 & 0xFF);
        pkt[p++] = (uint8_t)(bw10 >> 8);
        pkt[p++] = mp.cr;
        p += (int)putStr(pkt + p, mp.name, 15);
        sendChunk(meshChr, pkt, p);
    }
    uint8_t end = 0x9a;
    sendChunk(meshChr, &end, 1);
}

void sendMeshStats() {
    mesh_service::Stats s = mesh_service::stats();
    uint8_t pkt[1 + 7 * 4];
    int p = 0;
    pkt[p++] = 0x95;
    const uint32_t vals[7] = {s.rx, s.rxDropped, s.rxOtherChannel, s.rxDuplicate,
                              s.tx, s.txFailed, s.acksRx};
    for (uint32_t v : vals) { memcpy(pkt + p, &v, 4); p += 4; }
    meshNotify(pkt, p);
}

class MeshCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        std::string v = c->getValue();
        if (v.empty()) return;
        const uint8_t* p = (const uint8_t*)v.data();
        const size_t n = v.size();
        switch (p[0]) {
        case 0x01: meshReq |= MREQ_STATE; break;
        case 0x03: meshReq |= MREQ_HISTORY; break;
        case 0x04: meshReq |= MREQ_NODES; break;
        case 0x09: meshReq |= MREQ_STATS; break;
        case 0x0a: meshReq |= MREQ_PRESETS; break;
        case 0x0c: meshReq |= MREQ_CHANNELS; break;
        case 0x0d: {                                   // add / replace a channel
            if (n < 3) return;
            const uint8_t idx = p[1];
            const size_t nameLen = p[2];
            if (3 + nameLen + 1 > n) return;
            size_t keep = nameLen < sizeof(meshChanNamePending) - 1
                              ? nameLen : sizeof(meshChanNamePending) - 1;
            memcpy(meshChanNamePending, p + 3, keep);
            meshChanNamePending[keep] = 0;
            size_t pskLen = p[3 + nameLen];
            if (pskLen > mesh::MAX_PSK_LEN) return;
            if (3 + nameLen + 1 + pskLen > n) return;
            memcpy(meshChanPskPending, p + 3 + nameLen + 1, pskLen);
            meshChanPskLenPending = (uint8_t)pskLen;
            meshChanIdxPending = (int8_t)idx;
            break;
        }
        case 0x0f:                                     // position sharing
            if (n >= 3) {
                mesh_service::setSharesLocation(p[1], p[2] != 0);
                meshReq |= MREQ_CHANNELS;
            }
            break;
        case 0x0e:                                     // forget a channel
            if (n >= 2) {
                meshChanPskLenPending = 0xFF;          // delete marker
                meshChanIdxPending = (int8_t)p[1];
            }
            break;
        case 0x0b:                                     // set the modem preset
            if (n >= 2) meshPresetPending = (int8_t)p[1];
            break;
        case 0x10: meshReq |= MREQ_REGIONS; break;
        case 0x11:                                     // set the region
            if (n >= 2 && p[1] < mesh::REGION_COUNT) meshRegionPending = (int8_t)p[1];
            break;
        case 0x12:                                     // set TX power
            if (n >= 2 && (int8_t)p[1] >= 0) meshTxPending = (int8_t)p[1];
            break;
        case 0x13:                                     // pin the frequency slot
            if (n >= 2) meshSlotPending = p[1];
            break;
        case 0x02: {                                   // send a text message
            // [op][u32 dest][u8 channel][utf8]
            if (n < 6) return;
            if (meshSendCount >= MESH_SEND_QUEUE) return;   // task is behind
            StagedText& s = meshSendQ[meshSendCount];
            memcpy(&s.dest, p + 1, 4);
            s.channel = p[5];
            size_t tn = n - 6;
            if (tn > mesh::MAX_TEXT_LEN) tn = mesh::MAX_TEXT_LEN;
            memcpy(s.text, p + 6, tn);
            s.text[tn] = 0;
            meshSendCount++;
            break;
        }
        case 0x05: {                                   // rename this node
            if (n < 2) return;
            const size_t ln = p[1];
            if (1 + 1 + ln > n) return;
            size_t keep = ln < sizeof(meshSetLong) - 1 ? ln : sizeof(meshSetLong) - 1;
            memcpy(meshSetLong, p + 2, keep);
            meshSetLong[keep] = 0;
            size_t sn = n - 2 - ln;
            if (sn > sizeof(meshSetShort) - 1) sn = sizeof(meshSetShort) - 1;
            memcpy(meshSetShort, p + 2 + ln, sn);
            meshSetShort[sn] = 0;
            meshNamesPending = true;
            break;
        }
        case 0x06:                                     // radio on / off
            if (n >= 2) meshEnablePending = p[1] ? 1 : 0;
            break;
        case 0x07: {                                   // switch channel
            // [op][key] with an OPTIONAL name: zero-length means "follow the
            // modem preset", which is the interoperable default rather than a
            // malformed write.
            if (n < 2) return;
            meshSetKey = p[1];
            size_t cn = n - 2;
            if (cn > sizeof(meshSetChan) - 1) cn = sizeof(meshSetChan) - 1;
            memcpy(meshSetChan, p + 2, cn);
            meshSetChan[cn] = 0;
            meshChanPending = true;
            break;
        }
        case 0x08:                                     // mark read
            mesh_service::markAllRead();
            meshReq |= MREQ_STATE;
            break;
        }
    }
};

// Everything the phone asked of the mesh, run off the BLE host task. Called once
// per server-loop pass.
void serviceMeshRequests() {
    while (meshSendCount > 0) {
        // Copy first, then shrink the queue: a write callback can land between
        // these two statements, and it appends at meshSendCount.
        StagedText s = meshSendQ[0];
        for (int i = 1; i < meshSendCount; ++i) meshSendQ[i - 1] = meshSendQ[i];
        meshSendCount--;
        const uint32_t id = mesh_service::queueText(s.dest, s.text, s.channel);
        if (id) {
            uint8_t pkt[5] = {0x97};
            memcpy(pkt + 1, &id, 4);
            meshNotify(pkt, 5);
        } else {
            uint8_t nak = 0x98;
            meshNotify(&nak, 1);
        }
        meshReq |= MREQ_HISTORY | MREQ_STATE;   // show it in the app's list
    }
    if (meshNamesPending) {
        meshNamesPending = false;
        mesh_service::setNames(meshSetLong, meshSetShort);
        meshReq |= MREQ_STATE;
    }
    if (meshChanPending) {
        meshChanPending = false;
        mesh_service::setChannel(meshSetChan, meshSetKey);
        meshReq |= MREQ_STATE | MREQ_HISTORY | MREQ_NODES;
    }
    if (meshEnablePending >= 0) {
        mesh_service::setEnabled(meshEnablePending != 0);
        meshEnablePending = -1;
        meshReq |= MREQ_STATE;
    }
    if (meshChanIdxPending >= 0) {
        const uint8_t idx = (uint8_t)meshChanIdxPending;
        const uint8_t len = meshChanPskLenPending;
        meshChanIdxPending = -1;
        if (len == 0xFF) mesh_service::deletePrivateChannel(idx);
        else mesh_service::setPrivateChannel(idx, meshChanNamePending,
                                             meshChanPskPending, len);
        // The edit is staged again inside mesh_service and applied by the mesh
        // task, so the list the phone reads back is the one that took effect.
        meshReq |= MREQ_CHANNELS | MREQ_STATE | MREQ_HISTORY;
    }
    if (meshPresetPending >= 0) {
        mesh_service::setPreset((uint8_t)meshPresetPending);
        meshPresetPending = -1;
        // The frequency moves with the bandwidth, so the app needs the new state.
        meshReq |= MREQ_STATE;
    }
    if (meshRegionPending >= 0) {
        mesh_service::setRegion((uint8_t)meshRegionPending);
        meshRegionPending = -1;
        // A new band is a new mesh: the device forgets its neighbours and
        // messages, so the app must too.
        meshReq |= MREQ_STATE | MREQ_HISTORY | MREQ_NODES;
    }
    if (meshTxPending >= 0) {
        mesh_service::setTxPower((int8_t)meshTxPending);
        meshTxPending = -1;
        meshReq |= MREQ_STATE;
    }
    if (meshSlotPending >= 0) {
        mesh_service::setFreqSlot((uint8_t)meshSlotPending);
        meshSlotPending = -1;
        meshReq |= MREQ_STATE | MREQ_HISTORY | MREQ_NODES;
    }
    // Taken and cleared in one pass, before any of the streaming below: each
    // send yields for tens of milliseconds, and clearing a bit after that window
    // would drop a request the phone made during it.
    const uint8_t req = meshReq;
    if (!req) return;
    meshReq = (uint8_t)(meshReq & ~req);
    if (req & MREQ_STATE)   sendMeshState();
    if (req & MREQ_HISTORY) sendMeshHistory();
    if (req & MREQ_NODES)   sendMeshNodes();
    if (req & MREQ_STATS)   sendMeshStats();
    if (req & MREQ_PRESETS) sendMeshPresets();
    if (req & MREQ_REGIONS) sendMeshRegions();
    if (req & MREQ_CHANNELS) sendMeshChannels();
}

// Stream the device's map coverage to the phone (bounds of the embedded map +
// each downloaded /maps/*.ebm). Reads SD, so runs in the server task.
void sendMapList() {
    map_store::MapBounds mb[16];
    int n = map_store::listMaps(mb, 16);
    uint8_t begin = 0xC0;
    mapNotify(&begin, 1);
    for (int i = 0; i < n; ++i) {
        uint8_t pkt[34];
        pkt[0] = 0xC1;
        memcpy(pkt + 1, &mb[i].s, 8);
        memcpy(pkt + 9, &mb[i].w, 8);
        memcpy(pkt + 17, &mb[i].n, 8);
        memcpy(pkt + 25, &mb[i].e, 8);
        pkt[33] = mb[i].builtin ? 1 : 0;
        mapNotify(pkt, 34);
    }
    uint8_t end = 0xC2;
    mapNotify(&end, 1);
}

// Write the staged map/tile to SD + activate it. Runs in the server task.
void mapCommit() {
    bool tile = mapIsTile;
    bool ok = mapBuf && mapBufLen > 0 &&
              (tile ? map_store::saveTile(mapName, mapBuf, mapBufLen,
                                          mapVersion[0] ? mapVersion : nullptr)
                    : map_store::saveAndActivate(mapName, mapBuf, mapBufLen));
    uint32_t dt = millis() - mapStartMs;
    diag::log("%s commit %s: %u bytes in %.1fs", tile ? "tile" : "map",
              ok ? "ok" : "FAIL", (unsigned)mapBufLen, dt / 1000.0f);
    mapFreeBuf();
    mapIsTile = false;
    uint8_t r = ok ? 0xB1 : 0xBF;
    mapNotify(&r, 1);
}

// Stream the H3 tile ids already on the SD to the phone, so it can skip
// re-sending tiles it already delivered. Walks the card (the filename IS the
// id, so there is no index to read) and runs in the server task to stay off
// the BLE host thread. The phone asks on every Maps-screen open, so the timing
// is logged: this is the request path most likely to grow past the watchdog.
// poi=true: the same walk and framing for <id>.poi files, answered as
// 0xD3/0xD4/0xD5 (0x08). versions=true (asked as [0x07|0x08][0x01]): the begin
// carries a 0x01 flag byte and each id the record it was saved with, as
// "<id>=<record>" — or the bare id where the device has none (saved before
// this firmware, by an app that sends none, or changed over USB since).
void sendTileList(bool poi = false, bool versions = false) {
    // Out of PSRAM for the records: ids without them rather than no list at
    // all. Still the flagged begin, so the app goes on sending records for the
    // device to keep.
    const bool flagged = versions;
    uint32_t t0 = millis();
    ble_shared::TileListing tl;
    if (!ble_shared::lockTileList(poi, versions, tl)) return;
    const char (*ids)[24] = tl.ids;
    const char (*vers)[24] = tl.vers;
    const int n = tl.n;
    versions = vers != nullptr;
    uint32_t walkMs = millis() - t0;

    uint8_t begin[2] = {(uint8_t)(poi ? 0xD3 : 0xD0), 0x01};
    sendChunk(mapChr, begin, flagged ? 2 : 1);

    // Pack comma-separated ids into MTU-sized 0xD1 packets so a big list is a
    // handful of reliable notifications, not one per tile (that was slow).
    int cap = (int)negotiatedMTU - 3;      // leave ATT overhead
    if (cap < 20) cap = 20;
    if (cap > 240) cap = 240;
    uint8_t pkt[244];
    int len = 1;
    pkt[0] = poi ? 0xD4 : 0xD1;
    for (int i = 0; i < n; ++i) {
        char entry[24 + tile_versions::VER_MAX];
        int el = (versions && vers[i][0])
                     ? snprintf(entry, sizeof(entry), "%.22s=%s", ids[i], vers[i])
                     : snprintf(entry, sizeof(entry), "%.22s", ids[i]);
        if (el >= (int)sizeof(entry)) el = (int)sizeof(entry) - 1;
        if (len > 1 && len + 1 + el > cap) {    // flush the full packet
            sendChunk(mapChr, pkt, len);
            len = 1;
        }
        if (len > 1) pkt[len++] = ',';
        memcpy(pkt + len, entry, el);
        len += el;
    }
    if (len > 1) sendChunk(mapChr, pkt, len);

    uint8_t end = poi ? 0xD5 : 0xD2;
    sendChunk(mapChr, &end, 1);
    ble_shared::unlockTileList();
    diag::log("%s list: %d ids sent%s (walk %ums, total %ums)", poi ? "poi" : "tile",
              n, versions ? " with versions" : "", walkMs, millis() - t0);
}

// Write the PSRAM-staged firmware to the SD card as /firmware.bin, then reboot
// so the SD-update path (ui_dashboard applySdUpdate) flashes it on boot. This
// keeps the actual flash OUT of the BLE session entirely — the transfer just
// delivers the file, and flashing happens cleanly from SD after a reboot, which
// is far more reliable than flashing during/right after the BLE transfer.
// Runs in the server task; caller guards against SD contention with the recorder.
void otaCommit() {
    uint32_t t0 = millis();
    otaPhase = 2;                        // "installing" popup while we write SD
    bool ok = otaBuf && otaBufLen > 0 && ble_shared::saveFirmwareImage(otaBuf, otaBufLen);
    otaFreeBuf();
    if (ok) {
        otaNotify1(0xA1);                // app: received + saved, device installing
        diag::log("ota saved to SD in %.1fs — rebooting to flash from SD",
                  (millis() - t0) / 1000.0f);
        ble_shared::rebootForUpdate();   // reboot -> applySdUpdate flashes it
    } else {
        otaPhase = 0;
        uint8_t e[2] = {0xAF, 0xE0};     // SD write failed
        otaNotify(e, 2);
        diag::log("ota SD save FAILED");
    }
}

void begin() {
    // The legacy characteristics (NimBLE host task) and the link task share
    // the feature code below; these guard the parts that keep static state.
    sharedLocMutex = xSemaphoreCreateMutex();
    sharedTileMutex = xSemaphoreCreateMutex();
    sharedDashMutex = xSemaphoreCreateMutex();
    routeBuf = (char*)heap_caps_malloc(ROUTE_MAX, MALLOC_CAP_SPIRAM);
    NimBLEDevice::setMTU(247);   // ask for a large MTU for fast transfers

    // Fixed pairing: every app characteristic needs an authenticated (passkey,
    // Secure Connections, so bonded) link, and so does every CCCD — set before
    // the server starts, because the host reads the CCCD permission when it
    // registers the descriptors. A phone that isn't paired gets "insufficient
    // authentication" on its first read, write or subscribe, which is also
    // what makes iOS/Android start pairing on an unpaired device. Pushes below
    // are gated on phoneConnected (= admitted) as well.
    ble_pair_guard_harden();
    ble_pair_guard_set_refuse(refuseRepeatPairing);
    constexpr uint32_t kR = NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC |
                            NIMBLE_PROPERTY::READ_AUTHEN;
    constexpr uint32_t kW = NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC |
                            NIMBLE_PROPERTY::WRITE_AUTHEN;
    constexpr uint32_t kWNR = NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC |
                              NIMBLE_PROPERTY::WRITE_AUTHEN;

    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCb());
    NimBLEService* svc = server->createService(SVC_UUID);

    settingsChr = svc->createCharacteristic(
        CHR_SETTINGS,
        kR | kW | NIMBLE_PROPERTY::NOTIFY);
    settingsChr->setCallbacks(new SettingsCb());
    writeSettingsValue(settingsChr);

    statusChr = svc->createCharacteristic(
        CHR_STATUS, kR | NIMBLE_PROPERTY::NOTIFY);

    routeChr = svc->createCharacteristic(
        CHR_ROUTE, kW | NIMBLE_PROPERTY::NOTIFY);
    routeChr->setCallbacks(new RouteCb());

    ridesChr = svc->createCharacteristic(
        CHR_RIDES, kW | NIMBLE_PROPERTY::NOTIFY);
    ridesChr->setCallbacks(new RidesCb());

    otaChr = svc->createCharacteristic(
        CHR_OTA,
        kW | kWNR | NIMBLE_PROPERTY::NOTIFY);
    otaChr->setCallbacks(new OtaCb());

    sensorsChr = svc->createCharacteristic(
        CHR_SENSORS, kW | NIMBLE_PROPERTY::NOTIFY);
    sensorsChr->setCallbacks(new SensorsCb());

    mapChr = svc->createCharacteristic(
        CHR_MAP,
        kW | kWNR | NIMBLE_PROPERTY::NOTIFY);
    mapChr->setCallbacks(new MapCb());

    agnssChr = svc->createCharacteristic(
        CHR_AGNSS,
        kW | kWNR | NIMBLE_PROPERTY::NOTIFY);
    agnssChr->setCallbacks(new AgnssCb());

    dashChr = svc->createCharacteristic(
        CHR_DASH,
        kR | kW | NIMBLE_PROPERTY::NOTIFY,
        DASH_TEXT_MAX);   // a 4-page config outgrows the 512 B default
    dashChr->setCallbacks(new DashCb());
    writeDashValue(dashChr);

    meshChr = svc->createCharacteristic(
        CHR_MESH,
        kW | kWNR | NIMBLE_PROPERTY::NOTIFY);
    meshChr->setCallbacks(new MeshCb());

    mediaChr = svc->createCharacteristic(
        CHR_MEDIA,
        kW | kWNR | NIMBLE_PROPERTY::NOTIFY);
    mediaChr->setCallbacks(new MediaCb());

    workoutChr = svc->createCharacteristic(
        CHR_WORKOUT,
        kW | kWNR | NIMBLE_PROPERTY::NOTIFY);
    workoutChr->setCallbacks(new WorkoutCb());

    svc->start();

    // The phone link (docs/ble-link.md), a service of its own on the UUIDs
    // the unreleased LIVE stream used. Apps that predate it never look for
    // it, and keep to the characteristics above.
    phone_link::begin(server, kW | kWNR);

    // Pairing state comes straight from the bond store, so a device updated
    // from before fixed pairing boots paired to whatever phone(s) it already
    // had (see "Fixed pairing" above).
    phoneBonds = countPhoneBonds();
    {
        char n[sizeof(phoneNameCache)];
        snprintf(n, sizeof(n), "%s", settings::phoneName());
        memcpy(phoneNameCache, n, sizeof(n));
    }
    diag::log("ble: %d phone bond(s) of %d stored — %s", phoneBonds.load(),
              NimBLEDevice::getNumBonds(),
              phoneBonds.load() > 0 ? "paired, other phones refused"
                                    : "not paired, open to the first phone");
    applyAdvertising();

    Serial.println("[srv] GATT server advertising as OpenTrailPaper");
}

bool phonePaired() { return phoneBonds.load() > 0 && !unpairPending; }
int pairedPhoneCount() { return unpairPending ? 0 : phoneBonds.load(); }
const char* pairedPhoneName() { return phonePaired() ? phoneNameCache : ""; }
void requestUnpair() { unpairPending = true; }

void notePhoneName(const char* utf8) {
    if (!utf8 || !linkAdmitted) return;
    char folded[sizeof(phoneNameCache)];
    foldName(utf8, folded, sizeof(folded));
    if (!folded[0]) return;
    memcpy(phoneNameCache, folded, sizeof(folded));
    settings::setPhoneName(folded);
    diag::log("ble: paired phone is \"%s\"", folded);
}

void pushSettingsToPhone() { settingsDirty = true; }
void pushMapLayersToPhone() { mapLayersPushDue = true; }
void reportInterval() { intervalReportPending = true; }
void requestFastInterval() { noteBulk(); intervalReportPending = true; }

bool isPhoneConnected() { return phoneConnected; }
bool linkPending() { return linkUp && !linkAdmitted && !linkRefused; }
unsigned long phoneConnectionId() { return phoneConnectionEpoch.load(); }
bool linkRelaxed() { return linkIntervalLong; }
bool relaxedSleepAllowed() { return relaxedSleepOk; }
void setRelaxedSleepExperiment(bool on) {
    relaxedDropCount = 0;   // a re-armed run gets a fresh three strikes
    relaxedSleepOk = on;
    diag::log("ble: light-sleep-with-phone experiment %s", on ? "ARMED" : "off");
}

void mediaCommand(unsigned char cmd) { mediaCmdPending = cmd; }

unsigned char takeStopwatchCommand() {
    uint8_t op = 0;
    portENTER_CRITICAL(&liveSwMux);
    if (liveSwCount) {
        op = liveSwQueue[liveSwHead];
        liveSwHead = (liveSwHead + 1) % LIVE_SW_QUEUE;
        --liveSwCount;
    }
    portEXIT_CRITICAL(&liveSwMux);
    return op;
}

void publishLiveView(const LiveView& v) {
    portENTER_CRITICAL(&liveViewMux);
    // A change the app should see at once (not the running clock or the lap
    // averages, which the next 1 Hz frame carries anyway).
    uint8_t changed = 0;
    if (v.page != liveView.page || v.pageCount != liveView.pageCount ||
        v.screen != liveView.screen)
        changed |= ble_shared::LIVE_CHANGED_VIEW;
    if (v.swRunning != liveView.swRunning || v.swAccumMs != liveView.swAccumMs ||
        v.swStartMs != liveView.swStartMs || v.swLaps != liveView.swLaps)
        changed |= ble_shared::LIVE_CHANGED_STOPWATCH;
    liveView = v;
    portEXIT_CRITICAL(&liveViewMux);
    if (changed) liveViewChanges.fetch_or(changed);
}

unsigned int pairingCode() {
    // Self-expires: an abandoned iOS dialog never completes authentication,
    // and the panel must not wear a dead code into the ride.
    if (pairCode && millis() - pairCodeAtMs > 60000) pairCode = 0;
    return pairCode;
}

bool takeDashChanged() {
    if (!dashDirty) return false;
    dashDirty = false;
    return true;
}

// The popup shows whichever update runs: the legacy OTA characteristic's, or
// the link's TARGET_FIRMWARE upload (one at a time: each refuses while the
// other is busy).
bool updateInProgress() { return otaPhase != 0 || linkUpdPhase != 0; }
int updatePercent() {
    if (otaPhase == 1) return otaBufCap ? (int)((uint64_t)otaBufLen * 100 / otaBufCap) : 0;
    if (otaPhase == 0 && linkUpdPhase == 1)
        return linkUpdTotal ? (int)((uint64_t)linkUpdGot * 100 / linkUpdTotal) : 0;
    return 100;   // installing
}
const char* updatePhase() {
    const int ph = otaPhase ? otaPhase : linkUpdPhase;
    return ph == 2 ? "Installing" : "Downloading";
}

void task(void*) {
    uint32_t lastStatus = 0;
    for (;;) {
        servicePhoneInterval();
        serviceUnpair();
        // A link that hasn't proven it holds our bond doesn't get to sit on
        // the device's one phone slot (advertising stops while it's up). A
        // bonded phone encrypts within a second of connecting — the AMS
        // security request at 1.5 s makes sure of it even with no app open —
        // so 20 s is generous; unpaired, the rider needs time to type the
        // passkey, and the code sheet itself expires at 60 s.
        if (linkUp && !linkAdmitted &&
            millis() - connAtMs > (phoneBonds.load() > 0 ? 20000u : 90000u)) {
            uint16_t h = linkHandle;
            if (phoneBonds.load() > 0) kickLink(h, "never proved its bond");
            else {
                diag::log("ble: link never paired in 90 s — dropping it so "
                          "another phone can connect");
                NimBLEDevice::getServer()->disconnect(h);
            }
            connAtMs = millis();   // one request per window, not one per pass
        }
        if (!linkUp && advDirty) applyAdvertising();
        // A download that stalls (app backgrounded/killed without a clean
        // disconnect) must not leave the device stuck on the update popup.
        if (otaPhase == 1 && millis() - otaLastDataMs > 20000) {
            diag::log("ota download stalled — aborting");
            otaAbortIfDownloading();
        }
        // SD writers pause while the recorder OR a host computer owns the card.
        bool sdFree = !ride_recorder::isRecording() && !usb_storage::hostActive();
        if (otaCommitPending && sdFree) {
            otaCommitPending = false;    // write staged firmware to SD (off BLE
            otaCommit();                 // host, and not while the recorder owns SD)
        }
        if (mediaArtPending) {
            mediaArtPending = false;
            media::commitArt();
        }
        // From here on, every PUSH goes on exactly one path: the link when this
        // connection said Hello there, else the legacy characteristic. Answers
        // to legacy writes stay on the legacy characteristics.
        const bool viaLink = phone_link::active();
        static uint32_t trackSentVer = 0;
        if (mediaCapsPending) {
            mediaCapsPending = false;
            if (mediaChr && phoneConnected) {
                uint8_t pkt[2] = {0xA1, MEDIA_CAP_TONE_ART | MEDIA_CAP_AMS_TRACK |
                                        MEDIA_CAP_REMOTE};
                mediaChr->setValue(pkt, 2);
                mediaChr->notify();
                mediaTrackFeed = true;
                trackSentVer = 0;   // and tell it what AMS is playing right now
            }
        }
        // AMS delivers title, artist and album as separate updates; wait for a
        // quiet 300 ms so the app gets the whole track in one packet.
        if (mediaTrackFeed && mediaChr && phoneConnected && !viaLink) {
            uint32_t ver = media::amsTrackVersion();
            if (ver != 0 && ver != trackSentVer &&
                millis() - media::amsTrackChangedMs() >= 300) {
                trackSentVer = ver;
                const MediaState& st = media::get();
                uint8_t pkt[1 + 3 * (MEDIA_TRACK_FIELD_MAX + 1)];
                size_t n = 0;
                pkt[n++] = 0xA2;
                n += putTrackField(pkt + n, st.title, MEDIA_TRACK_FIELD_MAX);
                n += putTrackField(pkt + n, st.artist, MEDIA_TRACK_FIELD_MAX);
                n += putTrackField(pkt + n, st.album, MEDIA_TRACK_FIELD_MAX);
                mediaChr->setValue(pkt, n);
                mediaChr->notify();
            }
        }
        ams::tick();
        if (uint8_t mc = mediaCmdPending) {
            mediaCmdPending = 0;
            // AMS reaches every player and works with the app killed; the
            // app-notify path is the fallback when pairing was declined.
            if (!ams::sendCommand(mc) && phoneConnected) {
                if (viaLink) {
                    phone_link::pushMediaCommand(mc);
                } else if (mediaChr) {
                    uint8_t pkt[2] = {0xA0, mc};
                    mediaChr->setValue(pkt, 2);
                    mediaChr->notify();
                }
            }
        }
        if (dashApplyPending && sdFree) {
            dashApplyPending = false;
            const char* reason = nullptr;
            if (!ble_shared::applyDash(dashPendingText, &reason))
                diag::log("dash: app layout rejected (%s)", reason ? reason : "?");
            // Notify what the device ACTUALLY holds, whether the write took or
            // not, so the app's editor cannot sit showing a layout that was
            // rejected — and so a send that silently failed is visible. (A
            // link app gets the new layout as a DashConfig push instead.)
            if (dashChr && !viaLink) {
                writeDashValue(dashChr);
                // A notification carries at most MTU-3 bytes, and a multi-page
                // config no longer fits — the app was receiving the first 244
                // bytes, reparsing them as a smaller config, and wiping its
                // own editor. Short configs still go inline (an older app
                // understands nothing else); anything bigger sends a 1-byte
                // ping and the app reads the characteristic, which
                // reassembles long values properly.
                static char text[DASH_TEXT_MAX];
                size_t n = dash_config::currentText(text, sizeof(text));
                if (n <= 180) {
                    dashChr->notify();
                } else {
                    const uint8_t ping = 0x01;
                    dashChr->notify(&ping, 1);
                }
            }
        }
        if (mapCommitPending && sdFree) {
            mapCommitPending = false;    // write staged map to SD (off BLE host,
            mapCommit();                 // and never while the recorder owns SD)
        }
        if (mapListReq && sdFree) {
            mapListReq = false;          // enumerate SD maps (off BLE host)
            sendMapList();
        }
        if (tileListReq && sdFree) {
            tileListReq = false;         // enumerate SD H3 tile ids for dedup
            sendTileList(false, tileListVersions);
        }
        if (poiListReq && sdFree) {
            poiListReq = false;          // which tiles already have their POIs
            sendTileList(true, poiListVersions);
        }
        if (mapLayersPushDue && phoneConnected) {   // device-side layer edit
            mapLayersPushDue = false;
            if (viaLink) phone_link::push(phone_link::PUSH_MAP_LAYERS);
            else mapLayersNotify = true;
        }
        if (mapLayersNotify && mapChr && phoneConnected) {
            mapLayersNotify = false;
            const uint16_t m = settings::mapLayers();
            uint8_t pkt[3] = {0xE0, (uint8_t)(m & 0xFF), (uint8_t)(m >> 8)};
            sendChunk(mapChr, pkt, 3);
        }
        if (otaRebootPending) {          // OTA committed — reboot into new image
            vTaskDelay(pdMS_TO_TICKS(1500));   // let the success notify flush
            esp_restart();
        }
        if (settingsDirty && settingsChr && phoneConnected) {   // device-side edit -> mirror to phone
            settingsDirty = false;
            if (viaLink) {
                phone_link::push(phone_link::PUSH_SETTINGS);
            } else {
                writeSettingsValue(settingsChr);
                settingsChr->notify();
            }
        }
        // Service pending ride/route requests (they read the SD) — but only when
        // a host computer isn't using the card; otherwise leave them queued.
        if (!usb_storage::hostActive()) {
            if (rideReq == REQ_LIST) {
                rideReq = REQ_NONE;
                sendRideList();
            } else if (rideReq == REQ_DOWNLOAD) {
                rideReq = REQ_NONE;
                sendRide(rideReqName);
            } else if (rideReq == REQ_DELETE) {
                rideReq = REQ_NONE;
                deleteRide(rideReqName);
            } else if (rideReq == REQ_LOG) {
                rideReq = REQ_NONE;
                sendLog();
            } else if (rideReq == REQ_LOG_LIST) {
                rideReq = REQ_NONE;
                sendLogList();
            } else if (rideReq == REQ_LOG_FILE) {
                rideReq = REQ_NONE;
                sendLogFile(rideReqName);
            }
            if (routeReq == RREQ_LIST) {
                routeReq = RREQ_NONE;
                sendRouteList();
            } else if (routeReq == RREQ_DELETE) {
                routeReq = RREQ_NONE;
                deleteRoute(routeReqName);
            }
            // Save FIRST, list LAST: a list requested in the same breath
            // as an upload must include the file that upload created.
            if (workoutSavePending) {
                workoutSavePending = false;
                saveUploadedWorkout();
            }
            if (workoutReq == WREQ_DELETE) {
                workoutReq = WREQ_NONE;
                const bool ok = ble_shared::deleteWorkout(workoutReqName);
                workoutAck(ok, ok ? "deleted" : "delete failed");
            } else if (workoutReq == WREQ_LOAD) {
                workoutReq = WREQ_NONE;
                const char* reason = "";
                if (workout_service::load(workoutReqName, &reason))
                    workoutAck(true, workoutReqName);
                else
                    workoutAck(false, reason);
                workoutStatusDirty = true;
            } else if (workoutReq == WREQ_FETCH) {
                workoutReq = WREQ_NONE;
                sendWorkoutFile(workoutReqName);
            }
            if (workoutListPending) {
                workoutListPending = false;
                sendWorkoutList();
            }
        }
        if (sensorReq == SREQ_LIST) {
            sensorReq = SREQ_NONE;
            sendSensorList();
        }
        // Confirm route receipt to the phone (no SD access needed, so this runs
        // even while a host computer is using the card).
        if (routeChr && pendingRouteAck) {
            int a = pendingRouteAck; pendingRouteAck = 0;
            notifyByte2(a == 1 ? 0x23 : 0x24);
        }
        // Workout progress: pushed on any command, and each second while the
        // session clock runs so the app's screen counts down live. RAM only.
        {
            static uint32_t lastWkStatus = 0;
            // A device-side load or unload (the picker, its Back) changes
            // what the app should show too.
            static uint32_t seenWkVersion = 0;
            const uint32_t wkVer = workout_service::version();
            if (wkVer != seenWkVersion) {
                seenWkVersion = wkVer;
                workoutStatusDirty = true;
            }
            bool tick = workout_service::running() &&
                        millis() - lastWkStatus > 1000;
            if (viaLink) {
                workoutStatusDirty = false;   // the link task pushes its own
            } else if (workoutChr && phoneConnected && (workoutStatusDirty || tick)) {
                workoutStatusDirty = false;
                lastWkStatus = millis();
                sendWorkoutStatus();
            }
        }
        // Mesh messaging. Touches no SD and no flash, so it runs regardless of
        // who owns the card. A message that arrived (or a send that changed
        // state) raises 0x96 and the app asks for what it needs — pushing the
        // whole history on every packet would spend the link on data the app
        // usually already has.
        if (meshChr) {
            // phoneConnected first: takeChanged() CLEARS the flag, and consuming
            // it with nobody listening would throw the notification away.
            if (phoneConnected && mesh_service::takeChanged()) {
                if (viaLink) {
                    phone_link::push(phone_link::PUSH_MESH_CHANGED);
                } else {
                    uint8_t b = 0x96;
                    meshNotify(&b, 1);
                }
            }
            serviceMeshRequests();
        }

        serviceGattChanged();

        vTaskDelay(pdMS_TO_TICKS(100));
        if (!statusChr || millis() - lastStatus < 1000) continue;
        lastStatus = millis();

        // A clock the phone set keeps running off the system clock until GPS
        // has a fix and its own time (see applyPhoneLocation).
        if (phoneClockSet) {
            g_state.with([](RideState& st) {
                if (!st.gpsFix) { st.utc = time(nullptr); st.timeValid = true; }
            });
        }

        // While the app's sensor screen is open, push the list ~every 2 s so
        // discovered/connected changes show up live.
        static uint32_t lastSensorPush = 0;
        if (sensorsStreaming && phoneConnected && !phone_link::active() &&
            millis() - lastSensorPush > 2000) {
            lastSensorPush = millis();
            sendSensorList();
        }

        diag::flushToSD();   // persist buffered diagnostics (while recording,
                             // only once the ring is half full — see diag.cpp)

        // Safety net: make sure we're discoverable whenever no phone is
        // connected, in case a disconnect ever slips past onDisconnect.
        if (!linkUp && !unpairPending) {
            NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
            if (adv && !adv->isAdvertising()) NimBLEDevice::startAdvertising();
        }

        RideState s = g_state.snapshot();
        // { u8 flags, u8 battery, u8 sats, u8 hr, u16 power, u16 speed_x10,
        //   u16 remainingKm_x10 }
        uint8_t buf[10];
        uint8_t flags = (s.gpsFix ? 1 : 0) | (s.recording ? 2 : 0) |
                        (routes::active() ? 4 : 0);
        buf[0] = flags;
        buf[1] = s.batteryPercent;
        buf[2] = s.satellites;
        buf[3] = s.heartRateBpm;
        uint16_t pw = s.powerW == 0xFFFF ? 0 : s.powerW;
        buf[4] = pw & 0xFF; buf[5] = pw >> 8;
        uint16_t sp = (uint16_t)(s.speedKmh * 10);
        buf[6] = sp & 0xFF; buf[7] = sp >> 8;
        uint16_t rk = (uint16_t)(routes::remainingKm() * 10);
        buf[8] = rk & 0xFF; buf[9] = rk >> 8;

        statusChr->setValue(buf, sizeof(buf));
        // Admitted phone only: an unproven link may have subscribed (CCCDs
        // are open) but gets nothing pushed to it.
        // A link app gets this as LiveFrame RIDE instead.
        if (phoneConnected && !phone_link::active()) statusChr->notify();


    }
}

}  // namespace ble_server

// ---------------------------------------------------------------------------
// Shared with the phone link (ble_shared.h): one function per feature.

namespace ble_shared {

bool phoneAdmitted() { return phoneConnected; }
uint16_t phoneHandle() { return linkHandle; }
uint16_t mtu() { return negotiatedMTU; }
void noteBulk() { ::noteBulk(); }
bool sdFree() { return !ride_recorder::isRecording() && !usb_storage::hostActive(); }

void applySettings(const SettingsPatch& p) {
    if (p.hasFtp) settings::setFtpWatts(p.ftpW);
    if (p.hasTz) settings::setTzMinutes(p.tzMin);
    if (p.hasMiles) settings::setUseMiles(p.useMiles);
    if (p.hasBacklight) settings::setBacklight(p.backlight);
    if (p.hasClock24) settings::setClock24h(p.clock24h);
    if (p.hasUsbDrive) {
        settings::setUsbDrive(p.usbDrive);
        usb_storage::setDriveEnabled(p.usbDrive);   // apply immediately
    }
    // Out of range is refused (kept as is), not clamped: a typo of 210 mm
    // must not become an 800 mm wheel.
    if (p.hasWheel && !settings::setWheelCircMm(p.wheelMm))
        Serial.printf("[srv] wheel circumference %d mm refused\n", p.wheelMm);
    g_state.with([&](RideState& s) {
        s.ftpW = (uint16_t)settings::ftpWatts();
        s.tzMin = (int16_t)settings::tzMinutes();
        s.useMiles = settings::useMiles();
        s.clock24h = settings::clock24h();
    });
    Serial.printf("[srv] settings set: ftp=%d tz=%d miles=%d bl=%d wheel=%dmm\n",
                  settings::ftpWatts(), settings::tzMinutes(), settings::useMiles(),
                  settings::backlight(), settings::wheelCircMm());
}

void applyPhoneLocation(const PhoneFix& f) {
    const bool haveTime = f.haveUtc && f.utc > 1735689600UL;
    const double lat = f.latE7 / 1e7, lon = f.lonE7 / 1e7;
    // Warm-start the receiver (throttled + no-fix-only inside the task).
    gps_service::seedPosition(lat, lon, (time_t)f.utc, haveTime, 5000.0f);
    settings::setLastPosition(lat, lon);
    // Stash as the phone fallback fix (used when the device GPS is cold). The
    // estimator is shared by the legacy write (host task) and the link task.
    if (sharedLocMutex) xSemaphoreTake(sharedLocMutex, portMAX_DELAY);
    const uint32_t nowMs = millis();
    bool noGpsFix = false;
    static phone_motion::Estimator motion;
    motion.update(lat, lon, f.accM > 0 ? (float)f.accM : 0.0f, nowMs, f.speedMs,
                  f.courseDeg);
    g_state.with([&](RideState& st) {
        st.phoneSpeedKmh = motion.speedKmh(nowMs);
        st.phoneCourseValid = motion.courseValid();
        st.phoneCourseDeg = motion.courseDeg();
        st.phoneLat = lat;
        st.phoneLon = lon;
        if (f.haveAlt) st.phoneAltM = (float)f.altM;
        st.phoneAccM = f.accM > 0 ? (float)f.accM : 0.0f;
        st.phoneUtc = haveTime ? (time_t)f.utc : 0;
        // The clock (status bar, CLOCK field) otherwise waits for GPS time,
        // which a unit indoors after a reboot may not get for a long while.
        // The phone's network time is as good; GPS takes over once it has a fix.
        noGpsFix = !st.gpsFix;
        if (haveTime && noGpsFix) {
            st.utc = (time_t)f.utc;
            st.timeValid = true;
        }
        st.phoneFixValid = true;
        st.phoneFixMs = nowMs;
        // Speed/heading on the display while the phone is the source.
        speed_source::publish(st, nowMs);
    });
    if (sharedLocMutex) xSemaphoreGive(sharedLocMutex);
    // Keep the clock ticking between phone fixes (and after the app stops
    // sending): the server task's 1 Hz pass reads it back while GPS has no fix.
    if (haveTime && noGpsFix) {
        struct timeval tv = {(time_t)f.utc, 0};
        settimeofday(&tv, nullptr);
        phoneClockSet = true;
    }
}

bool legacyUpdateBusy() { return ble_server::otaPhase != 0; }

void setLinkUpdate(int phase, uint32_t got, uint32_t total) {
    ble_server::linkUpdGot = got;
    ble_server::linkUpdTotal = total;
    ble_server::linkUpdPhase = phase;
}

bool saveFirmwareImage(const uint8_t* img, size_t len) {
    if (!img || !len) return false;
    bool ok = false;
    sdLock();
    SD.remove("/firmware.bin");
    File f = SD.open("/firmware.bin", FILE_WRITE);
    if (f) {
        size_t wrote = 0;
        while (wrote < len) {
            size_t chunk = len - wrote < 8192 ? len - wrote : 8192;
            size_t w = f.write(img + wrote, chunk);
            if (w != chunk) break;
            wrote += w;
            // Same reason as the tile writes in map_store: a firmware image
            // is ~2 MB of uninterrupted SD work, and the task watchdog does
            // not care that it is making progress.
            if ((wrote & 0x3FFF) == 0) vTaskDelay(1);
        }
        f.close();
        ok = (wrote == len);
        if (!ok) SD.remove("/firmware.bin");
    }
    sdUnlock();
    return ok;
}

void rebootForUpdate() {
    diag::flushToSD();
    otaRebootPending = true;   // the server task restarts; applySdUpdate flashes
}

bool listDir(Dir d, void (*cb)(const char* name, uint32_t size, void* ctx), void* ctx) {
    const char* path = d == Dir::Rides ? RIDE_DIR
                     : d == Dir::Logs ? "/logs"
                     : d == Dir::Routes ? "/routes" : "/workouts";
    sdLock();
    File dir = SD.open(path);
    if (!dir) { sdUnlock(); return false; }
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (!f.isDirectory()) {
            const char* base = strrchr(f.name(), '/');
            base = base ? base + 1 : f.name();
            bool want = true;
            if (d == Dir::Logs) want = strstr(base, ".log") != nullptr;
            if (d == Dir::Workouts) want = base[0] != '.';   // macOS AppleDouble noise
            if (want) cb(base, (uint32_t)f.size(), ctx);
        }
        f.close();
    }
    dir.close();
    sdUnlock();
    return true;
}

static bool removeIn(const char* dir, const char* name) {
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    sdLock();
    const bool ok = SD.remove(path);
    sdUnlock();
    return ok;
}

bool deleteRide(const char* name) {
    const bool ok = removeIn(RIDE_DIR, name);
    Serial.printf("[srv] deleted ride %s\n", name);
    return ok;
}

bool deleteRoute(const char* name) {
    const bool ok = removeIn("/routes", name);
    // If the deleted route is the one currently loaded, drop it too.
    if (strcmp(routes::activeName(), name) == 0) routes::clearRoute();
    Serial.printf("[srv] deleted route %s\n", name);
    return ok;
}

bool deleteWorkout(const char* name) {
    const bool ok = removeIn("/workouts", name);
    if (ok) workout_service::requestListRefresh();   // the picker
    return ok;
}

WorkoutSave saveWorkout(const char* name, const char* data, size_t len, const char** reason) {
    char path[96];
    snprintf(path, sizeof(path), "/workouts/%s", name);
    sdLock();
    if (!SD.exists("/workouts")) SD.mkdir("/workouts");
    File f = SD.open(path, FILE_WRITE);
    bool ok = false;
    if (f) {
        ok = f.write((const uint8_t*)data, len) == len;
        f.close();
    }
    sdUnlock();
    if (!ok) return WorkoutSave::IoError;
    diag::log("workout: received %s (%u bytes) from the app", name, (unsigned)len);
    const bool loaded = workout_service::load(name, reason);
    workout_service::requestListRefresh();   // the picker lists it next time
    return loaded ? WorkoutSave::Ok : WorkoutSave::LoadFailed;
}

bool lockTileList(bool poi, bool versions, TileListing& out) {
    // PSRAM, and sized to the whole index. At 512 in .bss this was both 12 KB
    // of the internal RAM the display and BLE controller are short of, AND a
    // silent truncation: a rider past 512 tiles kept being offered tiles the
    // device already had, because the list the app dedups against stopped.
    constexpr int TILE_LIST_MAX = 4096;
    static_assert(tile_versions::VER_MAX == 24, "TileListing.vers is char[24]");
    if (sharedTileMutex) xSemaphoreTake(sharedTileMutex, portMAX_DELAY);
    static char (*ids)[24] = nullptr;
    if (!ids) {
        ids = (char(*)[24])heap_caps_malloc((size_t)TILE_LIST_MAX * 24, MALLOC_CAP_SPIRAM);
        if (!ids) {
            diag::log("tile list: no PSRAM for %d ids", TILE_LIST_MAX);
            if (sharedTileMutex) xSemaphoreGive(sharedTileMutex);
            return false;
        }
    }
    // Only for a versioned list, so firmware talking to an older app never
    // allocates them.
    static uint32_t* sizes = nullptr;
    static char (*vers)[tile_versions::VER_MAX] = nullptr;
    if (versions && !sizes) {
        sizes = (uint32_t*)heap_caps_malloc((size_t)TILE_LIST_MAX * 4, MALLOC_CAP_SPIRAM);
        vers = (char(*)[tile_versions::VER_MAX])heap_caps_malloc(
            (size_t)TILE_LIST_MAX * tile_versions::VER_MAX, MALLOC_CAP_SPIRAM);
        if (!sizes || !vers) {
            diag::log("tile list: no PSRAM for versions");
            heap_caps_free(sizes); heap_caps_free(vers);
            sizes = nullptr; vers = nullptr;
        }
    }
    if (!sizes) versions = false;
    const int n = map_store::listTileIds(ids, TILE_LIST_MAX, poi ? ".poi" : ".ebm",
                                         versions ? sizes : nullptr);
    if (versions) map_store::tileVersions(ids, sizes, n, poi, vers);
    out.ids = ids;
    out.vers = versions ? vers : nullptr;
    out.n = n;
    return true;
}

void unlockTileList() {
    if (sharedTileMutex) xSemaphoreGive(sharedTileMutex);
}

bool applyDash(const char* text, const char** reason) {
    if (sharedDashMutex) xSemaphoreTake(sharedDashMutex, portMAX_DELAY);
    const bool ok = dash_config::applyText(text, reason);
    if (sharedDashMutex) xSemaphoreGive(sharedDashMutex);
    if (ok) dashDirty = true;   // the UI task repaints with the new layout
    return ok;
}

void readLiveView(ble_server::LiveView& out) {
    portENTER_CRITICAL(&ble_server::liveViewMux);
    out = ble_server::liveView;
    portEXIT_CRITICAL(&ble_server::liveViewMux);
}

uint8_t takeLiveViewChanges() { return ble_server::liveViewChanges.exchange(0); }

void queueStopwatch(uint8_t op) { ble_server::liveSwPush(op); }

}  // namespace ble_shared
