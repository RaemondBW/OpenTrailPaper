#include "ble_sensors.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

#include "ride_state.h"
#include "settings.h"

namespace {

const NimBLEUUID SVC_HR("180D");
const NimBLEUUID CHR_HR("2A37");
const NimBLEUUID SVC_POWER("1818");
const NimBLEUUID CHR_POWER("2A63");
const NimBLEUUID SVC_CSC("1816");
const NimBLEUUID CHR_CSC("2A5B");

using ble_sensors::KIND_HR;
using ble_sensors::KIND_POWER;
using ble_sensors::KIND_CSC;
using ble_sensors::KIND_COUNT;
using SensorKind = ble_sensors::Kind;

struct Sensor {
    Sensor(const char* n, NimBLEUUID s, NimBLEUUID c) : name(n), svc(s), chr(c) {}
    const char* name;
    NimBLEUUID svc;
    NimBLEUUID chr;
    NimBLEAddress addr{};
    bool found = false;      // discovered by scan, awaiting connect
    bool connected = false;
    NimBLEClient* client = nullptr;
};

Sensor sensors[KIND_COUNT] = {
    {"HR", SVC_HR, CHR_HR},
    {"Power", SVC_POWER, CHR_POWER},
    {"Cadence", SVC_CSC, CHR_CSC},
};

// Crank state for cadence-from-power-meter and CSC. Event time is 1/1024 s.
struct CrankState {
    uint16_t revs = 0;
    uint16_t eventTime = 0;
    bool primed = false;
};
CrankState crankFromPower, crankFromCsc;

uint8_t cadenceFromCrank(CrankState& cs, uint16_t revs, uint16_t eventTime) {
    if (!cs.primed) {
        cs.revs = revs;
        cs.eventTime = eventTime;
        cs.primed = true;
        return 0xFF;
    }
    uint16_t dRevs = revs - cs.revs;
    uint16_t dTime = eventTime - cs.eventTime;  // wraps correctly, unsigned
    cs.revs = revs;
    cs.eventTime = eventTime;
    if (dTime == 0) return 0xFF;
    if (dRevs == 0) return 0;
    uint32_t rpm = (uint32_t)dRevs * 60 * 1024 / dTime;
    return rpm > 254 ? 254 : (uint8_t)rpm;
}

void onHrNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
    if (len < 2) return;
    uint8_t flags = data[0];
    uint16_t hr = (flags & 0x01) ? (uint16_t)(data[1] | (data[2] << 8)) : data[1];
    g_state.with([&](RideState& s) {
        s.heartRateBpm = hr > 254 ? 254 : (uint8_t)hr;
    });
}

// 3 s rolling power average for the hero display (design: "POWER · 3S").
uint16_t powerRing[8];
uint32_t powerRingTimes[8];
int powerRingHead = 0;

uint16_t rollingPower3s(uint16_t watts) {
    powerRing[powerRingHead] = watts;
    powerRingTimes[powerRingHead] = millis();
    powerRingHead = (powerRingHead + 1) % 8;
    uint32_t now = millis(), sum = 0, n = 0;
    for (int i = 0; i < 8; ++i) {
        if (powerRingTimes[i] && now - powerRingTimes[i] <= 3000) {
            sum += powerRing[i];
            n++;
        }
    }
    return n ? (uint16_t)(sum / n) : watts;
}

void onPowerNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
    if (len < 4) return;
    uint16_t flags = data[0] | (data[1] << 8);
    int16_t watts = (int16_t)(data[2] | (data[3] << 8));

    // Walk optional fields in flag order to reach crank revolution data.
    size_t off = 4;
    if (flags & 0x0001) off += 1;   // pedal power balance
    if (flags & 0x0004) off += 2;   // accumulated torque
    if (flags & 0x0010) off += 6;   // wheel revolution data (u32 + u16)
    uint8_t cad = 0xFF;
    if ((flags & 0x0020) && len >= off + 4) {  // crank revolution data
        uint16_t revs = data[off] | (data[off + 1] << 8);
        uint16_t t = data[off + 2] | (data[off + 3] << 8);
        cad = cadenceFromCrank(crankFromPower, revs, t);
    }

    uint16_t w = watts < 0 ? 0 : (uint16_t)watts;
    uint16_t w3s = rollingPower3s(w);
    g_state.with([&](RideState& s) {
        s.powerW = w;
        s.power3sW = w3s;
        if (cad != 0xFF) s.cadenceRpm = cad;
    });
}

void onCscNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
    if (len < 1) return;
    uint8_t flags = data[0];
    size_t off = 1;
    if (flags & 0x01) off += 6;  // wheel revolution data
    if ((flags & 0x02) && len >= off + 4) {
        uint16_t revs = data[off] | (data[off + 1] << 8);
        uint16_t t = data[off + 2] | (data[off + 3] << 8);
        uint8_t cad = cadenceFromCrank(crankFromCsc, revs, t);
        if (cad != 0xFF) {
            g_state.with([&](RideState& s) { s.cadenceRpm = cad; });
        }
    }
}

// Candidate registry for the Sensors screen
constexpr int MAX_CANDIDATES = 12;
ble_sensors::Candidate candidates[MAX_CANDIDATES];
int candidateCount = 0;
SemaphoreHandle_t candMutex = nullptr;
bool scanAlways = false;

void noteCandidate(const NimBLEAdvertisedDevice* dev, uint8_t kindsMask) {
    xSemaphoreTake(candMutex, portMAX_DELAY);
    std::string addrStr = dev->getAddress().toString();
    const char* addr = addrStr.c_str();
    int slot = -1;
    for (int i = 0; i < candidateCount; ++i) {
        if (strcmp(candidates[i].addr, addr) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0 && candidateCount < MAX_CANDIDATES) {
        slot = candidateCount++;
        snprintf(candidates[slot].addr, sizeof(candidates[slot].addr), "%s", addr);
    }
    if (slot >= 0) {
        candidates[slot].kindsMask |= kindsMask;
        candidates[slot].rssi = (int8_t)dev->getRSSI();
        std::string name = dev->getName();
        if (!name.empty()) {
            snprintf(candidates[slot].name, sizeof(candidates[slot].name), "%s",
                     name.c_str());
        }
    }
    xSemaphoreGive(candMutex);
}

class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        uint8_t mask = 0;
        for (int k = 0; k < KIND_COUNT; ++k) {
            if (dev->isAdvertisingService(sensors[k].svc)) mask |= 1 << k;
        }
        if (!mask) return;
        noteCandidate(dev, mask);

        std::string addr = dev->getAddress().toString();
        for (int k = 0; k < KIND_COUNT; ++k) {
            if (!(mask & (1 << k))) continue;
            Sensor& sensor = sensors[k];
            if (sensor.found || sensor.connected) continue;
            // Connect ONLY to the address the user explicitly paired for
            // this kind. With nothing paired we never auto-adopt a device
            // (that used to grab strangers' sensors). Pairing happens on
            // the Sensors screen.
            const char* saved = settings::sensorAddr(k);
            if (!saved[0] || strcasecmp(saved, addr.c_str()) != 0) continue;
            sensor.addr = dev->getAddress();
            sensor.found = true;
            Serial.printf("[ble] found paired %s sensor: %s\n", sensor.name,
                          addr.c_str());
        }
    }
} scanCallbacks;

void markDisconnected(SensorKind kind) {
    g_state.with([&](RideState& s) {
        switch (kind) {
            case KIND_HR:
                s.hrConnected = false;
                s.heartRateBpm = 0xFF;
                break;
            case KIND_POWER:
                s.powerConnected = false;
                s.powerW = 0xFFFF;
                s.power3sW = 0xFFFF;
                break;
            case KIND_CSC:
                s.cadenceConnected = false;
                break;
            default:
                break;
        }
    });
}

class ClientCallbacks : public NimBLEClientCallbacks {
public:
    explicit ClientCallbacks(SensorKind kind) : kind_(kind) {}
    void onDisconnect(NimBLEClient*, int reason) override {
        Serial.printf("[ble] %s disconnected (reason %d)\n",
                      sensors[kind_].name, reason);
        sensors[kind_].connected = false;
        sensors[kind_].found = false;  // rediscover on next scan
        markDisconnected(kind_);
    }

private:
    SensorKind kind_;
};

ClientCallbacks hrCb(KIND_HR), powerCb(KIND_POWER), cscCb(KIND_CSC);
ClientCallbacks* clientCbs[KIND_COUNT] = {&hrCb, &powerCb, &cscCb};

bool connectSensor(SensorKind kind) {
    Sensor& sensor = sensors[kind];
    if (!sensor.client) {
        sensor.client = NimBLEDevice::createClient();
        sensor.client->setClientCallbacks(clientCbs[kind], false);
        sensor.client->setConnectTimeout(5000);
    }
    if (!sensor.client->connect(sensor.addr)) {
        sensor.found = false;
        return false;
    }

    NimBLERemoteService* svc = sensor.client->getService(sensor.svc);
    NimBLERemoteCharacteristic* chr = svc ? svc->getCharacteristic(sensor.chr) : nullptr;
    if (!chr || !chr->canNotify()) {
        sensor.client->disconnect();
        sensor.found = false;
        return false;
    }

    bool ok = false;
    switch (kind) {
        case KIND_HR:     ok = chr->subscribe(true, onHrNotify); break;
        case KIND_POWER:  ok = chr->subscribe(true, onPowerNotify); break;
        case KIND_CSC:    ok = chr->subscribe(true, onCscNotify); break;
        default: break;
    }
    if (!ok) {
        sensor.client->disconnect();
        sensor.found = false;
        return false;
    }

    sensor.connected = true;
    g_state.with([&](RideState& s) {
        if (kind == KIND_HR) s.hrConnected = true;
        if (kind == KIND_POWER) s.powerConnected = true;
        if (kind == KIND_CSC) s.cadenceConnected = true;
    });
    Serial.printf("[ble] %s connected\n", sensor.name);
    return true;
}

}  // namespace

namespace ble_sensors {

void begin() {
    candMutex = xSemaphoreCreateMutex();
    NimBLEDevice::init("BikeGPS");
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&scanCallbacks, false);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(60);
}

void task(void*) {
    NimBLEScan* scan = NimBLEDevice::getScan();
    for (;;) {
        bool allConnected = true;
        for (auto& sensor : sensors) {
            if (!sensor.connected) allConnected = false;
        }

        if ((!allConnected || scanAlways) && !scan->isScanning()) {
            scan->start(5000, false, true);
        }

        // Connecting while a scan runs is unreliable; stop it first.
        for (int k = 0; k < KIND_COUNT; ++k) {
            if (sensors[k].found && !sensors[k].connected) {
                if (scan->isScanning()) scan->stop();
                connectSensor((SensorKind)k);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void setScanAlways(bool on) { scanAlways = on; }

int getCandidates(Candidate* out, int maxOut) {
    xSemaphoreTake(candMutex, portMAX_DELAY);
    int n = candidateCount < maxOut ? candidateCount : maxOut;
    for (int i = 0; i < n; ++i) {
        out[i] = candidates[i];
        out[i].connected = false;
        out[i].paired = false;
        for (int k = 0; k < KIND_COUNT; ++k) {
            if (sensors[k].connected &&
                strcasecmp(sensors[k].addr.toString().c_str(),
                           candidates[i].addr) == 0) {
                out[i].connected = true;
            }
            if (strcasecmp(settings::sensorAddr(k), candidates[i].addr) == 0) {
                out[i].paired = true;
            }
        }
    }
    xSemaphoreGive(candMutex);
    return n;
}

void pairCandidate(const char* addr) {
    xSemaphoreTake(candMutex, portMAX_DELAY);
    uint8_t mask = 0;
    for (int i = 0; i < candidateCount; ++i) {
        if (strcasecmp(candidates[i].addr, addr) == 0) {
            mask = candidates[i].kindsMask;
            break;
        }
    }
    xSemaphoreGive(candMutex);

    for (int k = 0; k < KIND_COUNT; ++k) {
        if (!(mask & (1 << k))) continue;
        settings::setSensorAddr(k, addr);
        // drop a different currently-connected device for this kind
        if (sensors[k].connected &&
            strcasecmp(sensors[k].addr.toString().c_str(), addr) != 0) {
            sensors[k].client->disconnect();
        }
        sensors[k].found = false;
    }
    Serial.printf("[ble] paired %s (mask 0x%x)\n", addr, mask);
}

void forgetAll() {
    for (int k = 0; k < KIND_COUNT; ++k) {
        settings::setSensorAddr(k, "");
        if (sensors[k].connected) sensors[k].client->disconnect();
        sensors[k].found = false;
    }
    Serial.println("[ble] pairings cleared");
}

}  // namespace ble_sensors
