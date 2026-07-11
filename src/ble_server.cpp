#include "ble_server.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <SD.h>
#include <esp_heap_caps.h>

#include "config.h"
#include "ride_state.h"
#include "ride_recorder.h"
#include "settings.h"
#include "routes.h"

namespace {

// Custom 128-bit UUIDs — keep in sync with the iOS app.
const char* SVC_UUID      = "b1c50000-9e0f-4b7a-9c6d-1f2e3a4b5c6d";
const char* CHR_SETTINGS  = "b1c50001-9e0f-4b7a-9c6d-1f2e3a4b5c6d";
const char* CHR_STATUS    = "b1c50002-9e0f-4b7a-9c6d-1f2e3a4b5c6d";
const char* CHR_ROUTE     = "b1c50003-9e0f-4b7a-9c6d-1f2e3a4b5c6d";
const char* CHR_RIDES     = "b1c50004-9e0f-4b7a-9c6d-1f2e3a4b5c6d";

NimBLECharacteristic* statusChr = nullptr;
NimBLECharacteristic* ridesChr = nullptr;

// Settings payload: little-endian { int16 ftpW, int16 tzMin }.
void writeSettingsValue(NimBLECharacteristic* c) {
    int16_t buf[2] = {(int16_t)settings::ftpWatts(),
                      (int16_t)settings::tzMinutes()};
    c->setValue((uint8_t*)buf, sizeof(buf));
}

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
            g_state.with([&](RideState& s) {
                s.ftpW = (uint16_t)settings::ftpWatts();
                s.tzMin = (int16_t)settings::tzMinutes();
            });
            Serial.printf("[srv] settings set: ftp=%d tz=%d\n",
                          settings::ftpWatts(), settings::tzMinutes());
        }
    }
};

// Route upload framing (each BLE write is one packet):
//   [0x01] start     : rest is the route name (UTF-8)
//   [0x02] data      : rest is raw GPX bytes, appended in order
//   [0x03] end track : finalize geometry -> parse + activate
//   [0x04] maneuver  : [i32 lat_e7][i32 lon_e7][utf8 instruction]
//   [0x05] end nav   : all maneuvers received -> raise nav prompt
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

class RouteCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        std::string v = c->getValue();
        if (v.empty()) return;
        uint8_t op = v[0];
        const char* payload = v.data() + 1;
        size_t plen = v.size() - 1;

        if (op == 0x01) {  // start
            routeLen = 0;
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
        } else if (op == 0x06) {  // list routes
            routeReq = RREQ_LIST;
        } else if (op == 0x07 && plen > 0) {  // delete route
            size_t n = plen < sizeof(routeReqName) - 1 ? plen
                                                       : sizeof(routeReqName) - 1;
            memcpy(routeReqName, payload, n);
            routeReqName[n] = 0;
            routeReq = RREQ_DELETE;
        }
    }
};

// Ride download (device -> phone). The phone writes a command; the task
// streams the response via notifications on the same characteristic.
//   Phone -> device:  [0x01] list  [0x02]<name> download  [0x03]<name> delete
//   Device -> phone:  list:     [0x01][u32 size][name] per ride, [0x03] end
//                     download:  [0x10][u32 total], [0x11]<bytes>…, [0x12] end
//                     delete ack: [0x13]        error (e.g. recording): [0x1F]
enum RideReq { REQ_NONE, REQ_LIST, REQ_DOWNLOAD, REQ_DELETE };
volatile RideReq rideReq = REQ_NONE;
char rideReqName[48];

// Windowed-transfer ack from the phone: [0x04][u16 nextExpectedSeq].
volatile bool rideAckPending = false;
volatile uint16_t rideAckSeq = 0;

class RidesCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        std::string v = c->getValue();
        if (v.empty()) return;
        if (v[0] == 0x01) {
            rideReq = REQ_LIST;
        } else if (v[0] == 0x04 && v.size() >= 3) {   // window ack
            rideAckSeq = (uint8_t)v[1] | ((uint8_t)v[2] << 8);
            rideAckPending = true;
        } else if ((v[0] == 0x02 || v[0] == 0x03) && v.size() > 1) {
            size_t n = v.size() - 1;
            if (n > sizeof(rideReqName) - 1) n = sizeof(rideReqName) - 1;
            memcpy(rideReqName, v.data() + 1, n);
            rideReqName[n] = 0;
            rideReq = v[0] == 0x02 ? REQ_DOWNLOAD : REQ_DELETE;
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
    File dir = SD.open(RIDE_DIR);
    if (!dir) { notifyByte(0x03); return; }
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
    notifyByte(0x03);
}

void sendRide(const char* name) {
    if (ride_recorder::isRecording() || !ride_recorder::sdMounted()) {
        notifyByte(0x1F);
        return;
    }
    char path[80];
    snprintf(path, sizeof(path), RIDE_DIR "/%s", name);
    File f = SD.open(path, FILE_READ);
    if (!f) { notifyByte(0x1F); return; }

    uint32_t total = f.size();
    // Payload per chunk must leave room for [0x11][u16 seq] and fit the MTU.
    int chunk = (int)negotiatedMTU - 3 - 3;    // -3 ATT overhead, -3 our header
    if (chunk < 16) chunk = 16;
    if (chunk > 180) chunk = 180;
    Serial.printf("[srv] download %s: total=%u, MTU=%u, chunk=%d\n", name,
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
            Serial.printf("[srv] download %s: disconnected at seq %u\n", name, seq);
            f.close();
            return;
        }
        if (resends > 200) {                // a chunk keeps failing — bail, don't hang
            Serial.printf("[srv] download %s: too many resends, aborting\n", name);
            f.close();
            return;
        }
        f.seek((uint32_t)seq * chunk);
        for (int i = 0; i < WINDOW; ++i) {
            int n = f.read(pkt + 3, chunk);
            if (n <= 0) { eof = true; break; }
            pkt[1] = seq & 0xFF;
            pkt[2] = seq >> 8;
            sendChunk(ridesChr, pkt, n + 3);
            seq++;
        }
        // Ask the app what it has, then wait for its ack.
        rideAckPending = false;
        uint8_t we = 0x14;                 // window-end / please-ack
        sendChunk(ridesChr, &we, 1);
        uint32_t t0 = millis();
        while (!rideAckPending && phoneConnected && millis() - t0 < 5000)
            vTaskDelay(pdMS_TO_TICKS(5));
        if (!rideAckPending) {
            Serial.printf("[srv] download %s: ACK TIMEOUT at seq %u\n", name, seq);
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
    Serial.printf("[srv] download %s: %u bytes in %u chunks, %d resends\n",
                  name, (unsigned)total, seq, resends);
}

void deleteRide(const char* name) {
    if (ride_recorder::isRecording()) { notifyByte(0x1F); return; }
    char path[80];
    snprintf(path, sizeof(path), RIDE_DIR "/%s", name);
    SD.remove(path);
    notifyByte(0x13);
    Serial.printf("[srv] deleted ride %s\n", name);
}

void sendRouteList() {
    if (!ride_recorder::sdMounted()) { notifyByte(0x21); return; }
    File dir = SD.open("/routes");
    if (!dir) { notifyByte(0x21); return; }
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
    notifyByte2(0x21);
}

void deleteRoute(const char* name) {
    char path[80];
    snprintf(path, sizeof(path), "/routes/%s", name);
    SD.remove(path);
    // If the deleted route is the one currently loaded, drop it too.
    if (strcmp(routes::activeName(), name) == 0) routes::clearRoute();
    notifyByte2(0x22);
    Serial.printf("[srv] deleted route %s\n", name);
}

}  // namespace

namespace ble_server {

class ServerCb : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
        Serial.printf("[srv] phone connected, MTU=%u\n", info.getMTU());
        negotiatedMTU = info.getMTU();
        phoneConnected = true;
    }
    void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override {
        Serial.printf("[srv] MTU negotiated: %u\n", mtu);
        negotiatedMTU = mtu;
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
        Serial.println("[srv] phone disconnected");
        phoneConnected = false;
        // Advertising stops once a central connects; restart it so the phone
        // can find and reconnect to the device after disconnecting.
        NimBLEDevice::startAdvertising();
    }
};

void begin() {
    routeBuf = (char*)heap_caps_malloc(ROUTE_MAX, MALLOC_CAP_SPIRAM);
    NimBLEDevice::setMTU(247);   // ask for a large MTU for fast transfers

    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(new ServerCb());
    NimBLEService* svc = server->createService(SVC_UUID);

    NimBLECharacteristic* settingsChr = svc->createCharacteristic(
        CHR_SETTINGS,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
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

    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(SVC_UUID);
    adv->setName("BikeGPS");
    adv->enableScanResponse(true);
    NimBLEDevice::startAdvertising();

    Serial.println("[srv] GATT server advertising as BikeGPS");
}

void task(void*) {
    uint32_t lastStatus = 0;
    for (;;) {
        // Service a pending ride/route request promptly (streams via notify).
        if (rideReq == REQ_LIST) {
            rideReq = REQ_NONE;
            sendRideList();
        } else if (rideReq == REQ_DOWNLOAD) {
            rideReq = REQ_NONE;
            sendRide(rideReqName);
        } else if (rideReq == REQ_DELETE) {
            rideReq = REQ_NONE;
            deleteRide(rideReqName);
        }
        if (routeReq == RREQ_LIST) {
            routeReq = RREQ_NONE;
            sendRouteList();
        } else if (routeReq == RREQ_DELETE) {
            routeReq = RREQ_NONE;
            deleteRoute(routeReqName);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
        if (!statusChr || millis() - lastStatus < 1000) continue;
        lastStatus = millis();

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
    }
}

}  // namespace ble_server
