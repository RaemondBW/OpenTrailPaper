#include "ble_server.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <SD.h>
#include <Update.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>

#include "config.h"
#include "ride_state.h"
#include "ride_recorder.h"
#include "settings.h"
#include "routes.h"
#include "gps_service.h"
#include "ble_sensors.h"
#include "map_select.h"
#include "map_store.h"
#include "sd_bus.h"
#include "usb_storage.h"
#include "dash_config.h"
#include "mesh_service.h"
#include "diag.h"

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

NimBLECharacteristic* statusChr = nullptr;
NimBLECharacteristic* sensorsChr = nullptr;
NimBLECharacteristic* ridesChr = nullptr;
NimBLECharacteristic* otaChr = nullptr;

// millis() of the last BULK traffic on the link — file streaming, list sends,
// OTA / map / route uploads. The connection-interval governor in task() holds
// the fast interval while this is recent and relaxes it once the link goes
// quiet. The phone's periodic location seeds and small command writes are
// deliberately NOT counted: seeds flow for entire no-fix stretches (which
// already force the fast interval), and counting every two-dozen-byte write
// would pin the link fast forever.
volatile uint32_t lastBulkMs = 0;
inline void noteBulk() { lastBulkMs = millis(); }

// MEASURED link state, for power_mgmt: true only while the phone is connected
// at a genuinely long interval (>= 100 ms, read back from the link — not from
// our own request, which iOS is free to reject). This is the gate that lets
// the CPU light-sleep with the phone attached: at 30 ms the controller's
// drifty RC sleep clock missed anchor points and killed the link by
// supervision timeout (2026-08-02, 1231 drops); at 150-300 ms the drift
// budget is 10x wider. Refreshed 1 Hz by the governor in task().
volatile bool linkIntervalLong = false;

// Light-sleep-while-connected experiment switch, DEFAULT OFF. Run 2026-08-19:
// even at the relaxed 150-300 ms interval, the first sleep killed the link in
// 4 s and the latch tripped after three supervision timeouts in 41 s — the
// internal 150 kHz RC simply cannot wake the controller on target, at any
// interval we can ask for. Left in place (console `sleepexp on`, not
// persisted) so the experiment is one command to re-run after a controller
// clock fix (external 32 kHz crystal — needs a lib-builder rebuild), instead
// of an archaeology dig. The latch below still guards a re-armed run: three
// reason-520 drops while sleeping-relaxed turn it back off for the boot.
volatile int relaxedDropCount = 0;
volatile bool relaxedSleepOk = false;
volatile bool otaRebootPending = false;

NimBLECharacteristic* settingsChr = nullptr;

// Settings payload (little-endian): { int16 ftpW, int16 tzMin, u8 useMiles,
// u8 backlight }. Mirrored both ways so the app and device stay in sync.
void writeSettingsValue(NimBLECharacteristic* c) {
    uint8_t buf[8];
    int16_t ftp = (int16_t)settings::ftpWatts();
    int16_t tz = (int16_t)settings::tzMinutes();
    memcpy(buf, &ftp, 2);
    memcpy(buf + 2, &tz, 2);
    buf[4] = settings::useMiles() ? 1 : 0;
    buf[5] = (uint8_t)settings::backlight();
    buf[6] = settings::clock24h() ? 1 : 0;
    buf[7] = settings::usbDrive() ? 1 : 0;
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
        if (v.size() >= 4) {
            int16_t ftp = (int16_t)((uint8_t)v[0] | ((uint8_t)v[1] << 8));
            int16_t tz = (int16_t)((uint8_t)v[2] | ((uint8_t)v[3] << 8));
            settings::setFtpWatts(ftp);
            settings::setTzMinutes(tz);
            if (v.size() >= 6) {
                settings::setUseMiles(v[4] != 0);
                settings::setBacklight((uint8_t)v[5]);
            }
            if (v.size() >= 7) settings::setClock24h(v[6] != 0);
            if (v.size() >= 8) {
                settings::setUsbDrive(v[7] != 0);
                usb_storage::setDriveEnabled(v[7] != 0);   // apply immediately
            }
            g_state.with([&](RideState& s) {
                s.ftpW = (uint16_t)settings::ftpWatts();
                s.tzMin = (int16_t)settings::tzMinutes();
                s.useMiles = settings::useMiles();
                s.clock24h = settings::clock24h();
            });
            Serial.printf("[srv] settings set: ftp=%d tz=%d miles=%d bl=%d\n",
                          settings::ftpWatts(), settings::tzMinutes(),
                          settings::useMiles(), settings::backlight());
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
constexpr size_t DASH_TEXT_MAX = 640;
char dashPendingText[DASH_TEXT_MAX];
volatile bool dashApplyPending = false;

// Serialize the CURRENT layout into the characteristic. Static buffer, same
// reason — onRead runs on the BLE task too.
void writeDashValue(NimBLECharacteristic* c) {
    static char text[DASH_TEXT_MAX];
    size_t n = dash_config::currentText(text, sizeof(text));
    c->setValue((const uint8_t*)text, n);
}

class DashCb : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        writeDashValue(c);
    }
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        std::string v = c->getValue();
        if (v.empty() || v.size() >= DASH_TEXT_MAX) return;
        memcpy(dashPendingText, v.data(), v.size());
        dashPendingText[v.size()] = 0;
        dashApplyPending = true;
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
            int32_t latE7, lonE7;
            memcpy(&latE7, payload, 4);
            memcpy(&lonE7, payload + 4, 4);
            uint32_t utc = 0;
            bool haveTime = false;
            if (plen >= 12) { memcpy(&utc, payload + 8, 4); haveTime = utc > 1735689600UL; }
            int16_t altM = 0;
            bool haveAlt = false;
            if (plen >= 14) { memcpy(&altM, payload + 12, 2); haveAlt = true; }
            // The app has always sent its horizontal accuracy here; we used to
            // stop parsing before it. The recorder needs it to decide whether a
            // phone position is good enough to go in the ride file.
            int16_t accM = 0;
            if (plen >= 16) memcpy(&accM, payload + 14, 2);
            double lat = latE7 / 1e7, lon = lonE7 / 1e7;
            // Warm-start the receiver (throttled + no-fix-only inside the task).
            gps_service::seedPosition(lat, lon, (time_t)utc, haveTime, 5000.0f);
            settings::setLastPosition(lat, lon);
            // Stash as the phone fallback fix (used when the device GPS is cold).
            uint32_t nowMs = millis();
            g_state.with([&](RideState& st) {
                st.phoneLat = lat;
                st.phoneLon = lon;
                if (haveAlt) st.phoneAltM = (float)altM;
                st.phoneAccM = accM > 0 ? (float)accM : 0.0f;
                st.phoneUtc = haveTime ? (time_t)utc : 0;
                st.phoneFixValid = true;
                st.phoneFixMs = nowMs;
            });
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
volatile bool phoneConnected = false;   // updated by ServerCb

// Best-effort paced notify. Returns false if notify() never succeeded
// (the packet was NOT delivered). Never aborts the transfer.
bool sendChunk(NimBLECharacteristic* chr, const uint8_t* data, size_t len) {
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
    sdLock();
    File dir = SD.open(RIDE_DIR);
    if (!dir) { sdUnlock(); notifyByte(0x03); return; }
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (!f.isDirectory()) {
            const char* base = strrchr(f.name(), '/');
            base = base ? base + 1 : f.name();
            uint8_t pkt[64];
            pkt[0] = 0x01;
            uint32_t sz = f.size();
            memcpy(pkt + 1, &sz, 4);
            size_t nl = strlen(base);
            if (nl > sizeof(pkt) - 5) nl = sizeof(pkt) - 5;
            memcpy(pkt + 5, base, nl);
            sendChunk(ridesChr, pkt, 5 + nl);
        }
        f.close();
    }
    dir.close();
    sdUnlock();
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
    sdLock();
    File dir = SD.open("/logs");
    if (!dir) { sdUnlock(); notifyByte(0x31); return; }
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (!f.isDirectory()) {
            const char* base = strrchr(f.name(), '/');
            base = base ? base + 1 : f.name();
            if (strstr(base, ".log")) {
                uint8_t pkt[64];
                pkt[0] = 0x30;
                uint32_t sz = f.size();
                memcpy(pkt + 1, &sz, 4);
                size_t nl = strlen(base);
                if (nl > sizeof(pkt) - 5) nl = sizeof(pkt) - 5;
                memcpy(pkt + 5, base, nl);
                sendChunk(ridesChr, pkt, 5 + nl);
            }
        }
        f.close();
    }
    dir.close();
    sdUnlock();
    notifyByte(0x31);
}

void sendLogFile(const char* name) {
    if (ride_recorder::isRecording() || !ride_recorder::sdMounted()) {
        notifyByte(0x1F);
        return;
    }
    diag::flushToSD();
    char path[64];
    snprintf(path, sizeof(path), "/logs/%.40s", name);
    streamFileWindowed(path, name);
}

void deleteRide(const char* name) {
    if (ride_recorder::isRecording()) { notifyByte(0x1F); return; }
    char path[80];
    snprintf(path, sizeof(path), RIDE_DIR "/%s", name);
    sdLock();
    SD.remove(path);
    sdUnlock();
    notifyByte(0x13);
    Serial.printf("[srv] deleted ride %s\n", name);
}

void sendRouteList() {
    if (!ride_recorder::sdMounted()) { notifyByte(0x21); return; }
    sdLock();
    File dir = SD.open("/routes");
    if (!dir) { sdUnlock(); notifyByte(0x21); return; }
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (!f.isDirectory()) {
            const char* base = strrchr(f.name(), '/');
            base = base ? base + 1 : f.name();
            uint8_t pkt[64];
            pkt[0] = 0x20;
            size_t nl = strlen(base);
            if (nl > sizeof(pkt) - 1) nl = sizeof(pkt) - 1;
            memcpy(pkt + 1, base, nl);
            sendChunk(routeChr, pkt, 1 + nl);
        }
        f.close();
    }
    dir.close();
    sdUnlock();
    notifyByte2(0x21);
}

void deleteRoute(const char* name) {
    char path[80];
    snprintf(path, sizeof(path), "/routes/%s", name);
    sdLock();
    SD.remove(path);
    sdUnlock();
    // If the deleted route is the one currently loaded, drop it too.
    if (strcmp(routes::activeName(), name) == 0) routes::clearRoute();
    notifyByte2(0x22);
    Serial.printf("[srv] deleted route %s\n", name);
}

}  // namespace

namespace ble_server {

// Defined further down (after the OTA/map transfer state); called from
// onDisconnect so an interrupted transfer can't wedge the device.
void otaAbortIfDownloading();
void mapAbortReceive();

class ServerCb : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* srv, NimBLEConnInfo& info) override {
        negotiatedMTU = info.getMTU();
        phoneConnected = true;
        diag::log("phone connected: MTU=%u interval=%.1fms", info.getMTU(),
                  info.getConnInterval() * 1.25f);
        // Ask for a fast connection interval (15-30 ms) so large transfers
        // (ride download, OTA) aren't throttled to one packet per ~slow tick.
        // The governor in task() relaxes this to 150-300 ms once the device is
        // riding (GPS fix held) and the link has gone quiet, and snaps it back
        // for transfers — treat the connect itself as activity so the app's
        // opening sync runs at full speed.
        lastBulkMs = millis();
        srv->updateConnParams(info.getConnHandle(), 12, 24, 0, 400);
    }
    void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override {
        diag::log("MTU negotiated: %u", mtu);
        negotiatedMTU = mtu;
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
        diag::log("phone disconnected (reason %d)", reason);
        // A supervision timeout (520) while the CPU was allowed to sleep on the
        // relaxed link is the experiment's failure signature — count it, and
        // pull the plug after three. Other reasons (531 = the app closed the
        // link on purpose) say nothing about sleep and don't count.
        if (reason == 520 && linkIntervalLong && relaxedSleepOk) {
            if (++relaxedDropCount >= 3) {
                relaxedSleepOk = false;
                diag::log("ble: light sleep with phone DISABLED "
                          "(3 supervision timeouts on the relaxed link)");
            }
        }
        linkIntervalLong = false;
        phoneConnected = false;
        // Recover from a transfer interrupted by the disconnect, so the device
        // doesn't stay frozen on the update popup / refuse the next attempt.
        otaAbortIfDownloading();
        mapAbortReceive();
        if (sensorsStreaming) {   // app's sensor screen can't still be open
            sensorsStreaming = false;
            ble_sensors::setScanAlways(false);
        }
        // Advertising stops once a central connects; restart it so the phone
        // can find and reconnect to the device after disconnecting.
        NimBLEDevice::startAdvertising();
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
//   device notifies: [0xB0] ready, [0xB1] saved, [0xB4][u32 received], [0xBF][err]
//   tile-list reply: [0xD0] begin, [0xD1]<h3id chars> per tile, [0xD2] end
NimBLECharacteristic* mapChr = nullptr;
uint8_t* mapBuf = nullptr;
volatile uint32_t mapBufLen = 0, mapBufCap = 0;
char mapName[52] = "";
volatile bool mapCommitPending = false;
volatile bool mapIsTile = false;         // staged buffer is an H3 tile, not a whole map
volatile bool mapListReq = false;
volatile bool tileListReq = false;
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
            size_t nl = n - 5 < sizeof(mapName) - 1 ? n - 5 : sizeof(mapName) - 1;
            memcpy(mapName, p + 5, nl);
            mapName[nl] = 0;
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
            tileListReq = true;
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
//
//   Device -> phone (notify):
//     [0x90] state    flags, node number, frequency, channel, our names
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
    meshNotify(pkt, p);
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
              (tile ? map_store::saveTile(mapName, mapBuf, mapBufLen)
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
void sendTileList() {
    // PSRAM, and sized to the whole index. At 512 in .bss this was both 12 KB of
    // the internal RAM the display and BLE controller are short of, AND a silent
    // truncation: a rider past 512 tiles kept being offered tiles the device
    // already had, because the list the app dedups against stopped there.
    constexpr int TILE_LIST_MAX = 4096;
    static char (*ids)[24] = nullptr;
    if (!ids) {
        ids = (char(*)[24])heap_caps_malloc((size_t)TILE_LIST_MAX * 24,
                                            MALLOC_CAP_SPIRAM);
        if (!ids) { diag::log("tile list: no PSRAM for %d ids", TILE_LIST_MAX); return; }
    }
    uint32_t t0 = millis();
    int n = map_store::listTileIds(ids, TILE_LIST_MAX);
    uint32_t walkMs = millis() - t0;

    uint8_t begin = 0xD0;
    sendChunk(mapChr, &begin, 1);

    // Pack comma-separated ids into MTU-sized 0xD1 packets so a big list is a
    // handful of reliable notifications, not one per tile (that was slow).
    int cap = (int)negotiatedMTU - 3;      // leave ATT overhead
    if (cap < 20) cap = 20;
    if (cap > 240) cap = 240;
    uint8_t pkt[244];
    int len = 1;
    pkt[0] = 0xD1;
    for (int i = 0; i < n; ++i) {
        int idl = (int)strlen(ids[i]);
        if (idl > 22) idl = 22;
        if (len > 1 && len + 1 + idl > cap) {   // flush the full packet
            sendChunk(mapChr, pkt, len);
            len = 1;
        }
        if (len > 1) pkt[len++] = ',';
        memcpy(pkt + len, ids[i], idl);
        len += idl;
    }
    if (len > 1) sendChunk(mapChr, pkt, len);

    uint8_t end = 0xD2;
    sendChunk(mapChr, &end, 1);
    diag::log("tile list: %d ids sent (walk %ums, total %ums)", n, walkMs, millis() - t0);
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
    bool ok = false;
    if (otaBuf && otaBufLen > 0) {
        sdLock();
        SD.remove("/firmware.bin");
        File f = SD.open("/firmware.bin", FILE_WRITE);
        if (f) {
            size_t wrote = 0;
            while (wrote < otaBufLen) {
                size_t chunk = otaBufLen - wrote < 8192 ? otaBufLen - wrote : 8192;
                size_t w = f.write(otaBuf + wrote, chunk);
                if (w != chunk) break;
                wrote += w;
                // Same reason as the tile writes in map_store: a firmware image
                // is ~2 MB of uninterrupted SD work on this task, and the task
                // watchdog does not care that it is making progress.
                if ((wrote & 0x3FFF) == 0) vTaskDelay(1);
            }
            f.close();
            ok = (wrote == otaBufLen);
            if (!ok) SD.remove("/firmware.bin");
        }
        sdUnlock();
    }
    otaFreeBuf();
    if (ok) {
        otaNotify1(0xA1);                // app: received + saved, device installing
        diag::log("ota saved to SD in %.1fs — rebooting to flash from SD",
                  (millis() - t0) / 1000.0f);
        diag::flushToSD();
        otaRebootPending = true;         // reboot -> applySdUpdate flashes it
    } else {
        otaPhase = 0;
        uint8_t e[2] = {0xAF, 0xE0};     // SD write failed
        otaNotify(e, 2);
        diag::log("ota SD save FAILED");
    }
}

void begin() {
    routeBuf = (char*)heap_caps_malloc(ROUTE_MAX, MALLOC_CAP_SPIRAM);
    NimBLEDevice::setMTU(247);   // ask for a large MTU for fast transfers

    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCb());
    NimBLEService* svc = server->createService(SVC_UUID);

    settingsChr = svc->createCharacteristic(
        CHR_SETTINGS,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    settingsChr->setCallbacks(new SettingsCb());
    writeSettingsValue(settingsChr);

    statusChr = svc->createCharacteristic(
        CHR_STATUS, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    routeChr = svc->createCharacteristic(
        CHR_ROUTE, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    routeChr->setCallbacks(new RouteCb());

    ridesChr = svc->createCharacteristic(
        CHR_RIDES, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    ridesChr->setCallbacks(new RidesCb());

    otaChr = svc->createCharacteristic(
        CHR_OTA,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
    otaChr->setCallbacks(new OtaCb());

    sensorsChr = svc->createCharacteristic(
        CHR_SENSORS, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    sensorsChr->setCallbacks(new SensorsCb());

    mapChr = svc->createCharacteristic(
        CHR_MAP,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
    mapChr->setCallbacks(new MapCb());

    agnssChr = svc->createCharacteristic(
        CHR_AGNSS,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
    agnssChr->setCallbacks(new AgnssCb());

    dashChr = svc->createCharacteristic(
        CHR_DASH,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    dashChr->setCallbacks(new DashCb());
    writeDashValue(dashChr);

    meshChr = svc->createCharacteristic(
        CHR_MESH,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
    meshChr->setCallbacks(new MeshCb());

    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(SVC_UUID);
    adv->setName("BikeGPS");
    adv->enableScanResponse(true);
    NimBLEDevice::startAdvertising();

    Serial.println("[srv] GATT server advertising as BikeGPS");
}

void pushSettingsToPhone() { settingsDirty = true; }

bool isPhoneConnected() { return phoneConnected; }
bool linkRelaxed() { return linkIntervalLong; }
bool relaxedSleepAllowed() { return relaxedSleepOk; }
void setRelaxedSleepExperiment(bool on) {
    relaxedDropCount = 0;   // a re-armed run gets a fresh three strikes
    relaxedSleepOk = on;
    diag::log("ble: light-sleep-with-phone experiment %s", on ? "ARMED" : "off");
}

bool takeDashChanged() {
    if (!dashDirty) return false;
    dashDirty = false;
    return true;
}

bool updateInProgress() { return otaPhase != 0; }
int updatePercent() {
    if (otaPhase == 1) return otaBufCap ? (int)((uint64_t)otaBufLen * 100 / otaBufCap) : 0;
    return 100;   // installing
}
const char* updatePhase() { return otaPhase == 2 ? "Installing" : "Downloading"; }

void task(void*) {
    uint32_t lastStatus = 0;
    for (;;) {
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
        if (dashApplyPending && sdFree) {
            dashApplyPending = false;
            const char* reason = nullptr;
            if (dash_config::applyText(dashPendingText, &reason)) {
                dashDirty = true;            // repaint with the new layout
            } else {
                diag::log("dash: app layout rejected (%s)", reason ? reason : "?");
            }
            // Notify what the device ACTUALLY holds, whether the write took or
            // not, so the app's editor cannot sit showing a layout that was
            // rejected — and so a send that silently failed is visible.
            if (dashChr) {
                writeDashValue(dashChr);
                dashChr->notify();
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
            sendTileList();
        }
        if (otaRebootPending) {          // OTA committed — reboot into new image
            vTaskDelay(pdMS_TO_TICKS(1500));   // let the success notify flush
            esp_restart();
        }
        if (settingsDirty && settingsChr) {   // device-side edit -> mirror to phone
            settingsDirty = false;
            writeSettingsValue(settingsChr);
            settingsChr->notify();
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
        // Mesh messaging. Touches no SD and no flash, so it runs regardless of
        // who owns the card. A message that arrived (or a send that changed
        // state) raises 0x96 and the app asks for what it needs — pushing the
        // whole history on every packet would spend the link on data the app
        // usually already has.
        if (meshChr) {
            // phoneConnected first: takeChanged() CLEARS the flag, and consuming
            // it with nobody listening would throw the notification away.
            if (phoneConnected && mesh_service::takeChanged()) {
                uint8_t b = 0x96;
                meshNotify(&b, 1);
            }
            serviceMeshRequests();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
        if (!statusChr || millis() - lastStatus < 1000) continue;
        lastStatus = millis();

        // While the app's sensor screen is open, push the list ~every 2 s so
        // discovered/connected changes show up live.
        static uint32_t lastSensorPush = 0;
        if (sensorsStreaming && phoneConnected && millis() - lastSensorPush > 2000) {
            lastSensorPush = millis();
            sendSensorList();
        }

        diag::flushToSD();   // persist buffered diagnostics (while recording,
                             // only once the ring is half full — see diag.cpp)

        // Safety net: make sure we're discoverable whenever no phone is
        // connected, in case a disconnect ever slips past onDisconnect.
        if (!phoneConnected) {
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
        statusChr->notify();

        // Connection-interval governor. The 15-30 ms interval requested at
        // connect exists for transfers; holding it through a whole ride wakes
        // both radios ~33x a second to exchange nothing but the 1 Hz status
        // notify. Once the device is riding (a fix held for 10 s — the same
        // signal the rider asked for: "fix means I'm out riding, not syncing")
        // and the link has been quiet for 8 s, ask for 150-300 ms. Snap back
        // the moment a transfer starts or the fix drops (no fix = the phone's
        // location seeds are the device's eyes — keep them prompt).
        //
        // iOS is the arbiter: a peripheral can only REQUEST parameters, so
        // both requests stay inside Apple's accessory rules (interval ratio,
        // timeout >= 3x interval) or they'd be rejected wholesale. Requests
        // are rate-limited — iOS ignores a peripheral that nags.
        {
            static bool connSlow = false;
            static uint32_t fixSinceMs = 0;
            static uint32_t lastReqMs = 0;
            if (!s.gpsFix) fixSinceMs = 0;
            else if (!fixSinceMs) fixSinceMs = millis() | 1;
            if (!phoneConnected) {
                connSlow = false;   // next connect starts fast (onConnect asks)
                linkIntervalLong = false;
            } else {
                const bool wantSlow = fixSinceMs &&
                                      millis() - fixSinceMs > 10000 &&
                                      millis() - lastBulkMs > 8000;
                NimBLEServer* srv = NimBLEDevice::getServer();
                if (srv && srv->getConnectedCount() > 0) {
                    NimBLEConnInfo pi = srv->getPeerInfo(0);
                    // What power_mgmt acts on is the MEASURED interval, not
                    // our request — iOS may reject or renegotiate, and
                    // sleeping against a real 30 ms link is exactly the
                    // failure this experiment must not reproduce. 80 units
                    // = 100 ms.
                    linkIntervalLong = pi.getConnInterval() >= 80;
                    if (wantSlow != connSlow &&
                        millis() - lastReqMs > (wantSlow ? 15000 : 3000)) {
                        connSlow = wantSlow;
                        lastReqMs = millis();
                        if (wantSlow) {
                            srv->updateConnParams(pi.getConnHandle(), 120, 240, 0, 400);
                            diag::log("ble: conn interval relaxed (riding, link idle)");
                        } else {
                            srv->updateConnParams(pi.getConnHandle(), 12, 24, 0, 400);
                            diag::log("ble: conn interval fast (%s)",
                                      s.gpsFix ? "transfer" : "no fix");
                        }
                    }
                }
            }
        }
    }
}

}  // namespace ble_server
