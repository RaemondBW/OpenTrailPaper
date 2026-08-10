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
    char make[32] = "";      // "Manufacturer Model" from Device Info Service
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

// Last HR measurement payload, for the `sensors` console command.
ble_sensors::HrPacket lastHr{};

void onHrNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
    if (len < 2) return;
    uint8_t flags = data[0];
    const bool wide = flags & 0x01;
    // A 16-bit value lives in data[1..2]; the old `len < 2` guard alone let a
    // 2-byte packet with the wide bit set read one byte past the payload.
    if (wide && len < 3) return;
    uint16_t hr = wide ? (uint16_t)(data[1] | (data[2] << 8)) : data[1];

    lastHr.flags = flags;
    lastHr.len = (uint8_t)len;
    lastHr.bpm = hr;
    lastHr.atMs = millis();

    // Diagnostic: the raw payload, so "a heart rate shows when I'm not wearing
    // the strap" is settled from the bytes instead of guessed at. Flags bit 2
    // says the strap supports contact detection, bit 1 says whether it has
    // contact right now.
    //
    // Do not trust bit 1 to catch an unworn strap: a damp one conducts, reports
    // contact=yes, and invents a plausible heart rate that decays as it dries
    // (observed: a strap on the bench sliding 165 -> 125 bpm over six minutes).
    // The RR intervals do not corroborate it either — the strap derives the BPM
    // from them, so phantom beats give a perfectly self-consistent pair.
    static uint32_t lastLog = 0;
    if (millis() - lastLog > 2000) {
        lastLog = millis();
        char hex[40];
        int p = 0;
        for (size_t i = 0; i < len && i < 12 && p < (int)sizeof(hex) - 3; ++i)
            p += snprintf(hex + p, sizeof(hex) - p, "%02X ", data[i]);
        diag::log("hr: len=%u flags=0x%02X contact=%s bpm=%u [%s]", (unsigned)len,
                  flags,
                  (flags & 0x04) ? ((flags & 0x02) ? "yes" : "NO") : "unsupported",
                  hr, hex);
    }

    g_state.with([&](RideState& s) {
        s.heartRateBpm = hr > 254 ? 254 : (uint8_t)hr;
        s.hrMs = millis();
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
    // The crank-data FLAG is what makes this meter a cadence source — not
    // whether a number can be derived right now. A stationary crank sends
    // revs=0 and time=0 forever (observed: "20 00 00 00 00 00 00 00"), so
    // cadenceFromCrank correctly returns nothing and the sensor was never
    // marked connected until the rider happened to be pedalling.
    const bool hasCrank = (flags & 0x0020) && len >= off + 4;
    static uint16_t lastRevs = 0;
    static bool haveLastRevs = false;
    bool crankStopped = false;
    if (hasCrank) {
        uint16_t revs = data[off] | (data[off + 1] << 8);
        uint16_t t = data[off + 2] | (data[off + 3] << 8);
        cad = cadenceFromCrank(crankFromPower, revs, t);
        // Not turning: report a real zero rather than holding the last value.
        crankStopped = haveLastRevs && revs == lastRevs;
        lastRevs = revs;
        haveLastRevs = true;
    }

    uint16_t w = watts < 0 ? 0 : (uint16_t)watts;
    addPowerSample(w);
    uint16_t w3s = power3sAvg();
    g_state.with([&](RideState& s) {
        s.powerW = w;
        s.power3sW = w3s;
        s.powerMs = millis();
        // A power meter with crank data IS a cadence sensor. Marking it
        // connected here is what makes the rest of the device agree: the
        // dashboard field, the menu's sensor line and the status bar all read
        // cadenceConnected, so without this a meter supplying cadence still
        // reported "Cadence --" and the field never counted as live.
        if (hasCrank) {
            s.cadenceConnected = true;
            s.cadenceMs = millis();
            if (cad != 0xFF) s.cadenceRpm = cad;
            else if (crankStopped) s.cadenceRpm = 0;
        }
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
            g_state.with([&](RideState& s) {
                s.cadenceRpm = cad;
                s.cadenceMs = millis();
                s.cadenceConnected = true;   // any CSC device is a cadence source
            });
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
// Set by the task whenever it is hunting for a paired sensor that isn't
// connected. Read by power_mgmt at 1 Hz — see radioBusy() in the header.
volatile bool huntingRadio = false;

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
        // diag::log, not Serial: a drop happens mid-ride with no console
        // attached, and the SD log is the only place it can be read afterwards.
        // reason 520 (0x208) is an HCI supervision timeout — the sensor stopped
        // answering (asleep, out of range) rather than closing the link.
        diag::log("%s sensor disconnected (reason %d)", sensors[kind_].name,
                  reason);
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

    // Read the Device Information Service (0x180A) for a human-readable make:
    // Manufacturer Name (0x2A29) + Model Number (0x2A24). Many sensors have no
    // advertised name (they show as a MAC), so this is what tells you the vendor.
    sensor.make[0] = 0;
    if (NimBLERemoteService* dis = sensor.client->getService(NimBLEUUID((uint16_t)0x180A))) {
        char mfr[24] = "", mdl[24] = "";
        if (auto* c = dis->getCharacteristic(NimBLEUUID((uint16_t)0x2A29))) {
            if (c->canRead()) snprintf(mfr, sizeof(mfr), "%s", c->readValue().c_str());
        }
        if (auto* c = dis->getCharacteristic(NimBLEUUID((uint16_t)0x2A24))) {
            if (c->canRead()) snprintf(mdl, sizeof(mdl), "%s", c->readValue().c_str());
        }
        if (mfr[0] && mdl[0]) snprintf(sensor.make, sizeof(sensor.make), "%s %s", mfr, mdl);
        else if (mfr[0])      snprintf(sensor.make, sizeof(sensor.make), "%s", mfr);
        else if (mdl[0])      snprintf(sensor.make, sizeof(sensor.make), "%s", mdl);
    }
    // Remember the best identity we have (persisted), so this sensor shows its
    // vendor/model next time even before it reconnects, and across reboots.
    {
        const char* best = sensor.make[0] ? sensor.make : sensor.advName;
        if (best[0]) settings::setSensorName(kind, best);
    }

    g_state.with([&](RideState& s) {
        if (kind == KIND_HR) s.hrConnected = true;
        if (kind == KIND_POWER) s.powerConnected = true;
        if (kind == KIND_CSC) s.cadenceConnected = true;
    });
    static const char* kn[KIND_COUNT] = {"HR", "power", "cadence"};
    diag::log("%s connected: %s [%s] make='%s', notify %s", kn[kind], sensor.advName,
              sensor.addr.toString().c_str(), sensor.make, ok ? "on" : "off");
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
        // "All connected" means every PAIRED sensor is connected. A kind the
        // user never paired (no saved address) can never connect, so counting
        // it here would leave allConnected=false forever and keep the radio
        // active-scanning for the entire ride (recording holds wantScan true) —
        // burning power hunting for a power meter / cadence sensor that doesn't
        // exist. Only paired kinds gate scanning; once they're up, scanning stops.
        bool allConnected = true;
        for (int k = 0; k < KIND_COUNT; ++k) {
            bool paired = settings::sensorAddr(k)[0] != 0;
            if (paired && !sensors[k].connected) allConnected = false;
        }

        // Only scan when it's actually useful: while recording, while the
        // Sensors screen is open, or for a short window after the user
        // interacts. Otherwise a paired-but-absent sensor (e.g. an HR strap
        // you're not wearing) would keep the radio scanning forever and drain
        // the battery while the device just sits idle.
        bool wantScan = !allConnected &&
                        (scanAlways || ride_recorder::isRecording() ||
                         millis() - lastActivityMs < 30000);

        // A sensor that is actually coming back — the meter waking on the first
        // pedal stroke, a strap re-wetting — starts advertising within seconds,
        // so the first minute of a hunt looks continuously. After that the
        // missing sensor probably isn't coming back this ride (left at home,
        // flat cell) and holding the radio awake for hours would cost the ~25%
        // that light sleep saves, so drop to a 5 s look every 10 s: still
        // reconnects within about ten seconds of the sensor reappearing.
        static uint32_t huntStartMs = 0;
        if (!wantScan) huntStartMs = 0;
        else if (!huntStartMs) huntStartMs = millis();
        uint32_t hunted = huntStartMs ? millis() - huntStartMs : 0;
        // scanAlways means the user is sitting on a Sensors screen watching the
        // list fill in — never duty-cycle that.
        bool lookNow = wantScan &&
                       (scanAlways || hunted < 60000 || (hunted / 5000) % 2 == 0);

        bool pendingConnect = false;
        for (int k = 0; k < KIND_COUNT; ++k)
            if (sensors[k].found && !sensors[k].connected) pendingConnect = true;

        // Keep the radio awake for the whole hunt — the scan AND the connect
        // attempts that follow it. Both need the controller listening on time.
        huntingRadio = lookNow || pendingConnect;

        if (lookNow && !scan->isScanning()) {
            scan->start(5000, false, true);
        } else if (!lookNow && scan->isScanning()) {
            scan->stop();
        }

        // Connecting while a scan runs is unreliable; stop it first.
        for (int k = 0; k < KIND_COUNT; ++k) {
            if (sensors[k].found && !sensors[k].connected) {
                if (scan->isScanning()) scan->stop();
                connectSensor((SensorKind)k);
            }
        }

        // Invalidate a reading whose packets have stopped arriving. Nothing
        // else does this while the LINK is still up: markDisconnected only runs
        // from the BLE disconnect callback, and ui_dashboard's staleness sweep
        // operates on its own render snapshot (g_state.snapshot()), so the
        // recorder and the phone never saw it. A strap that is connected but
        // off-body therefore left its last BPM in RideState forever — frozen on
        // the dashboard, and written into every FIT record and the ride's HR
        // average as if the rider were still wearing it.
        //
        // Only the VALUES are cleared, not the connected flags: onHrNotify does
        // not re-raise hrConnected, so clearing that here would grey the field
        // until the next full reconnect.
        constexpr uint32_t kStaleMs = 15000;   // ui_dashboard greys out at the same age
        g_state.with([&](RideState& s) {
            uint32_t now = millis();
            if (s.heartRateBpm != 0xFF && now - s.hrMs > kStaleMs)
                s.heartRateBpm = 0xFF;
            if (s.powerW != 0xFFFF && now - s.powerMs > kStaleMs)
                s.powerW = s.power3sW = 0xFFFF;
            if (s.cadenceRpm != 0xFF && now - s.cadenceMs > kStaleMs)
                s.cadenceRpm = 0xFF;
        });

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
bool radioBusy() { return huntingRadio; }

HrPacket lastHrPacket() { return lastHr; }

void links(Link* out) {
    for (int k = 0; k < KIND_COUNT; ++k) {
        Link& l = out[k];
        memset(&l, 0, sizeof(l));
        snprintf(l.kind, sizeof(l.kind), "%s", sensors[k].name);
        snprintf(l.pairedAddr, sizeof(l.pairedAddr), "%s", settings::sensorAddr(k));
        l.connected = sensors[k].connected;
        if (l.connected) {
            snprintf(l.liveAddr, sizeof(l.liveAddr), "%s",
                     sensors[k].addr.toString().c_str());
        }
        const char* nm = sensors[k].make[0]         ? sensors[k].make
                       : settings::sensorName(k)[0] ? settings::sensorName(k)
                       : sensors[k].advName[0]      ? sensors[k].advName
                                                    : "";
        snprintf(l.name, sizeof(l.name), "%s", nm);
    }
}

bool disconnect(int kind) {
    if (kind < 0 || kind >= KIND_COUNT) return false;
    if (!sensors[kind].connected || !sensors[kind].client) return false;
    // connected/found are cleared by ClientCallbacks::onDisconnect, which also
    // blanks the displayed value — don't duplicate that here.
    sensors[kind].client->disconnect();
    return true;
}

// The "Manufacturer Model" read from a connected sensor's Device Info Service,
// or nullptr if that address isn't a connected sensor / had no DIS.
static const char* makeForAddr(const char* addr) {
    for (int k = 0; k < KIND_COUNT; ++k) {
        if (sensors[k].make[0] &&
            strcasecmp(sensors[k].addr.toString().c_str(), addr) == 0) {
            return sensors[k].make;
        }
    }
    return nullptr;
}

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
        // Prefer the human-readable make once we've connected and read the DIS.
        if (const char* mk = makeForAddr(out[i].addr)) {
            snprintf(out[i].name, sizeof(out[i].name), "%s", mk);
        }
    }
    xSemaphoreGive(candMutex);

    // A connected sensor stops advertising, so it drops out of the scan
    // candidates above — yet the user needs to see it as connected. Append a
    // persistent row for every paired kind whose address isn't already listed,
    // marking its live connection state. This keeps paired sensors visible
    // (connected or not) whether or not they're currently advertising.
    for (int k = 0; k < KIND_COUNT && n < maxOut; ++k) {
        const char* paddr = settings::sensorAddr(k);
        if (!paddr || !paddr[0]) continue;
        bool already = false;
        for (int i = 0; i < n; ++i) {
            if (strcasecmp(out[i].addr, paddr) == 0) { already = true; break; }
        }
        if (already) continue;
        Candidate& c = out[n++];
        memset(&c, 0, sizeof(c));
        snprintf(c.addr, sizeof(c.addr), "%s", paddr);
        // Live make (this session) > remembered name (NVS, survives reboot /
        // shows before reconnect) > advertised name > generic kind label.
        const char* nm = sensors[k].make[0]        ? sensors[k].make
                       : settings::sensorName(k)[0] ? settings::sensorName(k)
                       : sensors[k].advName[0]      ? sensors[k].advName
                                                    : sensors[k].name;
        snprintf(c.name, sizeof(c.name), "%s", nm);
        c.kindsMask = (uint8_t)(1 << k);
        c.paired = true;
        c.connected = sensors[k].connected &&
                      strcasecmp(sensors[k].addr.toString().c_str(), paddr) == 0;
        c.rssi = 0;
        // Fold in any other kinds paired to the same address (e.g. power+cadence).
        for (int k2 = k + 1; k2 < KIND_COUNT; ++k2) {
            if (strcasecmp(settings::sensorAddr(k2), paddr) == 0) {
                c.kindsMask |= (uint8_t)(1 << k2);
            }
        }
    }
    return n;
}

void pairCandidate(const char* addr) {
    xSemaphoreTake(candMutex, portMAX_DELAY);
    uint8_t mask = 0;
    char advName[32] = "";
    for (int i = 0; i < candidateCount; ++i) {
        if (strcasecmp(candidates[i].addr, addr) == 0) {
            mask = candidates[i].kindsMask;
            snprintf(advName, sizeof(advName), "%s", candidates[i].name);
            break;
        }
    }
    xSemaphoreGive(candMutex);

    for (int k = 0; k < KIND_COUNT; ++k) {
        if (!(mask & (1 << k))) continue;
        settings::setSensorAddr(k, addr);
        // Remember the advertised name now; upgraded to the DIS make on connect.
        if (advName[0]) settings::setSensorName(k, advName);
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

void forget(const char* addr) {
    if (!addr || !addr[0]) return;
    for (int k = 0; k < KIND_COUNT; ++k) {
        if (strcasecmp(settings::sensorAddr(k), addr) != 0) continue;
        settings::setSensorAddr(k, "");
        if (sensors[k].connected &&
            strcasecmp(sensors[k].addr.toString().c_str(), addr) == 0) {
            sensors[k].client->disconnect();
        }
        sensors[k].found = false;
    }
    Serial.printf("[ble] forgot %s\n", addr);
}

}  // namespace ble_sensors
