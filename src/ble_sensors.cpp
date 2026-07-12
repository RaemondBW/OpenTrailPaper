#include "ble_sensors.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

#include "ride_state.h"
#include "settings.h"
#include "ride_recorder.h"
#include "diag.h"

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
    const char* name;        // generic kind label ("HR"/"Power"/"Cadence")
    NimBLEUUID svc;
    NimBLEUUID chr;
    NimBLEAddress addr{};
    char advName[24] = "";   // the device's advertised name (e.g. assioma…)
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
// Sized for a fast meter (~10 Hz) over the 3 s window.
constexpr int PWR_RING = 40;
uint16_t powerRing[PWR_RING];
uint32_t powerRingTimes[PWR_RING];
int powerRingHead = 0;

void addPowerSample(uint16_t watts) {
    powerRing[powerRingHead] = watts;
    powerRingTimes[powerRingHead] = millis();
    powerRingHead = (powerRingHead + 1) % PWR_RING;
}

// Average of samples still within the last 3 s. Recomputed on every notify AND
// once a second by the task, so the value decays promptly when you ease off —
// otherwise a slow-notifying meter would leave a stale sample lingering > 3 s.
uint16_t power3sAvg() {
    uint32_t now = millis(), sum = 0, n = 0;
    for (int i = 0; i < PWR_RING; ++i) {
        if (powerRingTimes[i] && now - powerRingTimes[i] <= 3000) {
            sum += powerRing[i];
            n++;
        }
    }
    return n ? (uint16_t)(sum / n) : 0;
}

void onPowerNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
    if (len < 4) return;
    uint16_t flags = data[0] | (data[1] << 8);
    int16_t watts = (int16_t)(data[2] | (data[3] << 8));

    // Diagnostic: log the raw payload periodically so a power-reads-0 issue can
    // be inspected from the log (pedal, then download it).
    static uint32_t lastLog = 0;
    if (millis() - lastLog > 2000) {
        lastLog = millis();
        char hex[40];
        int p = 0;
        for (size_t i = 0; i < len && i < 12 && p < (int)sizeof(hex) - 3; ++i)
            p += snprintf(hex + p, sizeof(hex) - p, "%02X ", data[i]);
        diag::log("pwr: len=%u flags=0x%04X watts=%d [%s]", (unsigned)len, flags,
                  watts, hex);
    }

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
    addPowerSample(w);
    uint16_t w3s = power3sAvg();
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
uint32_t lastActivityMs = 0;

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
            std::string advName = dev->getName();
            snprintf(sensor.advName, sizeof(sensor.advName), "%s",
                     advName.empty() ? "(unnamed)" : advName.c_str());
            diag::log("found paired %s: %s [%s]", sensor.name, sensor.advName,
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
    static const char* kn[KIND_COUNT] = {"HR", "power", "cadence"};
    diag::log("%s connected: %s [%s], notify %s", kn[kind], sensor.advName,
              sensor.addr.toString().c_str(), ok ? "on" : "off");
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
    // Lower duty cycle than before (was 60/100 = 60% radio-on) to save power;
    // still finds sensors within a few seconds.
    scan->setInterval(160);
    scan->setWindow(48);
}

void task(void*) {
    NimBLEScan* scan = NimBLEDevice::getScan();
    for (;;) {
        bool allConnected = true;
        for (auto& sensor : sensors) {
            if (!sensor.connected) allConnected = false;
        }

        // Only scan when it's actually useful: while recording, while the
        // Sensors screen is open, or for a short window after the user
        // interacts. Otherwise a paired-but-absent sensor (e.g. an HR strap
        // you're not wearing) would keep the radio scanning forever and drain
        // the battery while the device just sits idle.
        bool wantScan = !allConnected &&
                        (scanAlways || ride_recorder::isRecording() ||
                         millis() - lastActivityMs < 30000);
        if (wantScan && !scan->isScanning()) {
            scan->start(5000, false, true);
        } else if (!wantScan && scan->isScanning()) {
            scan->stop();
        }

        // Connecting while a scan runs is unreliable; stop it first.
        for (int k = 0; k < KIND_COUNT; ++k) {
            if (sensors[k].found && !sensors[k].connected) {
                if (scan->isScanning()) scan->stop();
                connectSensor((SensorKind)k);
            }
        }

        // Keep the 3 s power average current even between notifications so it
        // decays within 3 s when you stop pedaling (not "much longer").
        if (sensors[KIND_POWER].connected) {
            uint16_t w3s = power3sAvg();
            g_state.with([&](RideState& s) {
                if (s.powerW != 0xFFFF) s.power3sW = w3s;
            });
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void setScanAlways(bool on) { scanAlways = on; }
void noteActivity() { lastActivityMs = millis(); }

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
