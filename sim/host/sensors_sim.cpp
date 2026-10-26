// Virtual BLE sensors for the firmware's own sensor code (src/ble_sensors.cpp):
// each advertises its standard service and sends standard measurement packets
// — Heart Rate 0x2A37, Cycling Power 0x2A63 (with crank data), CSC 0x2A5B,
// a Varia radar — so pairing, decoding, drop-outs and reconnects are the
// firmware's real paths. Driven by the control channel's `sensor` command.
#include <Arduino.h>

#include "json.h"
#include "sim.h"
#include "sim_ble.h"
#include "sim_clock.h"

#include <cmath>
#include <thread>

namespace {

struct VSensor {
    std::string kind;
    std::shared_ptr<simble::SimPeripheral> per;
    NimBLEUUID svc, chr;
    double bpm = 0, watts = 0, cadence = 0, speedKmh = 0;
    std::vector<int> radar;   // metres behind, nearest first
    bool sending = true;
    // cumulative CSC/power counters
    double crankRevs = 0, wheelRevs = 0;
    uint16_t lastCrankT = 0, lastWheelT = 0;
    uint8_t radarSeq = 0;
    uint32_t wheelMm = 2105;
};

std::mutex g_m;
std::vector<std::shared_ptr<VSensor>> g_sensors;

const NimBLEUUID kHrSvc((uint16_t)0x180D), kHrChr((uint16_t)0x2A37);
const NimBLEUUID kPwrSvc((uint16_t)0x1818), kPwrChr((uint16_t)0x2A63);
const NimBLEUUID kCscSvc((uint16_t)0x1816), kCscChr((uint16_t)0x2A5B), kCscFeat((uint16_t)0x2A5C);
const NimBLEUUID kRadarSvc("6a4e3200-667b-11e3-949a-0800200c9a66");
const NimBLEUUID kRadarChr("6a4e3203-667b-11e3-949a-0800200c9a66");
const NimBLEUUID kDis((uint16_t)0x180A), kMfr((uint16_t)0x2A29), kModel((uint16_t)0x2A24);

std::shared_ptr<simble::SimRemoteChar> addChar(simble::SimPeripheral& p, const NimBLEUUID& svc,
                                               const NimBLEUUID& chr, uint32_t props,
                                               const std::string& value = "") {
    auto c = std::make_shared<simble::SimRemoteChar>();
    c->svc = svc;
    c->uuid = chr;
    c->props = props;
    c->value.assign(value.begin(), value.end());
    p.chars.push_back(c);
    return c;
}

std::shared_ptr<VSensor> make(const std::string& kind, const std::string& addr, const std::string& name,
                              const std::string& make, const std::string& model) {
    auto s = std::make_shared<VSensor>();
    s->kind = kind;
    s->per = std::make_shared<simble::SimPeripheral>();
    auto& p = *s->per;
    p.addr = NimBLEAddress(addr, BLE_ADDR_RANDOM);
    p.name = name;
    if (kind == "hr") { s->svc = kHrSvc; s->chr = kHrChr; }
    else if (kind == "power") { s->svc = kPwrSvc; s->chr = kPwrChr; }
    else if (kind == "radar") { s->svc = kRadarSvc; s->chr = kRadarChr; }
    else { s->svc = kCscSvc; s->chr = kCscChr; }
    p.advUuids.push_back(s->svc);
    addChar(p, s->svc, s->chr, NIMBLE_PROPERTY::NOTIFY);
    if (kind == "csc" || kind == "speed") {
        uint16_t feat = kind == "speed" ? 0x0001 : 0x0003;   // wheel; wheel + crank
        addChar(p, kCscSvc, kCscFeat, NIMBLE_PROPERTY::READ, std::string{(char)(feat & 0xFF), (char)(feat >> 8)});
    }
    if (kind == "power") addChar(p, kPwrSvc, NimBLEUUID((uint16_t)0x2A65), NIMBLE_PROPERTY::READ,
                                 std::string{0x08, 0x00, 0x00, 0x00});   // crank revolution data supported
    addChar(p, kDis, kMfr, NIMBLE_PROPERTY::READ, make);
    addChar(p, kDis, kModel, NIMBLE_PROPERTY::READ, model);
    return s;
}

std::string u16(uint16_t v) { return std::string{(char)(v & 0xFF), (char)(v >> 8)}; }
std::string u32(uint32_t v) {
    return std::string{(char)(v & 0xFF), (char)((v >> 8) & 0xFF), (char)((v >> 16) & 0xFF), (char)(v >> 24)};
}

// Advance a revolution counter by `rpm` over dt seconds; returns the event
// time (1/1024 s) of the last whole revolution.
void revs(double& count, uint16_t& eventT, double rpm, double dt, double nowS) {
    if (rpm <= 0) return;
    double before = count;
    count += rpm / 60.0 * dt;
    if (floor(count) > floor(before)) {
        double frac = (count - floor(count)) / (rpm / 60.0);   // seconds since that rev
        eventT = (uint16_t)((uint32_t)((nowS - frac) * 1024.0) & 0xFFFF);
    }
}

void tickSensor(VSensor& s, double dt, double nowS) {
    std::string pkt;
    if (s.kind == "hr") {
        if (s.bpm <= 0) return;
        pkt = std::string{0x00, (char)(uint8_t)std::min(255.0, s.bpm)};
    } else if (s.kind == "power") {
        revs(s.crankRevs, s.lastCrankT, s.cadence, dt, nowS);
        uint16_t flags = 0x0020;   // crank revolution data present
        pkt = u16(flags) + u16((uint16_t)(int16_t)lround(s.watts)) + u16((uint16_t)((uint32_t)s.crankRevs & 0xFFFF)) +
              u16(s.lastCrankT);
    } else if (s.kind == "csc" || s.kind == "speed") {
        double wheelRpm = s.speedKmh / 3.6 / (s.wheelMm / 1000.0) * 60.0;
        revs(s.wheelRevs, s.lastWheelT, wheelRpm, dt, nowS);
        uint8_t flags = 0x01;
        pkt = std::string{(char)flags} + u32((uint32_t)s.wheelRevs) + u16(s.lastWheelT);
        if (s.kind == "csc") {
            revs(s.crankRevs, s.lastCrankT, s.cadence, dt, nowS);
            pkt[0] = 0x03;
            pkt += u16((uint16_t)((uint32_t)s.crankRevs & 0xFFFF)) + u16(s.lastCrankT);
        }
    } else if (s.kind == "radar") {
        s.radarSeq = (uint8_t)(((s.radarSeq >> 4) + 1) << 4 | 0x02);
        pkt = std::string{(char)s.radarSeq};
        int id = 1;
        for (int d : s.radar) {
            if (pkt.size() >= 1 + 3 * 6) break;
            pkt += (char)(0x80 | id++);
            pkt += (char)(uint8_t)std::max(1, std::min(254, d));
            pkt += (char)40;   // closing speed, km/h
        }
    }
    simble::peripheralNotify(s.per, s.svc, s.chr, (const uint8_t*)pkt.data(), pkt.size());
}

void loop() {
    pthread_setname_np("vsensors");
    uint32_t last = sim_clock::millis();
    for (;;) {
        sim_clock::sleepMs(250);
        uint32_t now = sim_clock::millis();
        static uint32_t accum = 0;
        accum += now - last;
        last = now;
        std::vector<std::shared_ptr<VSensor>> v;
        {
            std::lock_guard<std::mutex> lk(g_m);
            v = g_sensors;
        }
        bool second = accum >= 1000;
        for (auto& s : v) {
            if (!s->sending || !s->per->client) continue;
            if (s->kind == "radar") tickSensor(*s, 0.25, now / 1000.0);
            else if (second) tickSensor(*s, accum / 1000.0, now / 1000.0);
        }
        if (second) accum = 0;
    }
}

std::string defaultAddr(const std::string& kind) {
    if (kind == "hr") return "c0:ff:ee:00:00:01";
    if (kind == "power") return "c0:ff:ee:00:00:02";
    if (kind == "csc") return "c0:ff:ee:00:00:03";
    if (kind == "speed") return "c0:ff:ee:00:00:04";
    return "c0:ff:ee:00:00:05";
}

std::string defaultName(const std::string& kind) {
    if (kind == "hr") return "HRM-Sim";
    if (kind == "power") return "PWR-Sim";
    if (kind == "csc") return "CAD-Sim";
    if (kind == "speed") return "SPD-Sim";
    return "RTL-Sim";
}

}  // namespace

std::string sensorCommand(const json::Value& r) {
    static std::once_flag started;
    std::call_once(started, [] { std::thread(loop).detach(); });
    const std::string kind = r.str("kind", "hr");
    const std::string addr = r.str("addr", defaultAddr(kind));
    std::shared_ptr<VSensor> s;
    {
        std::lock_guard<std::mutex> lk(g_m);
        for (auto& x : g_sensors) if (x->per->addr.toString() == addr) s = x;
    }
    if (r.str("cmd") == "sensor_remove") {
        if (!s) return "\"ok\":false,\"error\":\"no such sensor\"";
        simble::peripheralGone(s->per, BLE_HS_ERR_HCI_BASE + BLE_ERR_CONN_SPVN_TMO);
        {
            std::lock_guard<std::mutex> lk(simble::peripheralsMutex());
            auto& ps = simble::peripherals();
            ps.erase(std::remove(ps.begin(), ps.end(), s->per), ps.end());
        }
        std::lock_guard<std::mutex> lk(g_m);
        g_sensors.erase(std::remove(g_sensors.begin(), g_sensors.end(), s), g_sensors.end());
        return "\"ok\":true";
    }
    if (!s) {
        s = make(kind, addr, r.str("name", defaultName(kind)), r.str("make", "Sim"), r.str("model", defaultName(kind)));
        {
            std::lock_guard<std::mutex> lk(simble::peripheralsMutex());
            simble::peripherals().push_back(s->per);
        }
        std::lock_guard<std::mutex> lk(g_m);
        g_sensors.push_back(s);
    }
    {
        std::lock_guard<std::mutex> lk(simble::peripheralsMutex());
        if (r.has("advertising")) s->per->advertising = r.boolean("advertising");
        if (r.has("rssi")) s->per->rssi = (int)r.num("rssi");
    }
    if (r.has("bpm")) s->bpm = r.num("bpm");
    if (r.has("watts")) s->watts = r.num("watts");
    if (r.has("cadence")) s->cadence = r.num("cadence");
    if (r.has("speed_kmh")) s->speedKmh = r.num("speed_kmh");
    if (r.has("sending")) s->sending = r.boolean("sending");
    if (r.has("targets")) {
        s->radar.clear();
        for (auto& t : r["targets"].a) s->radar.push_back((int)t.n);
    }
    if (r.boolean("drop")) simble::peripheralGone(s->per, BLE_HS_ERR_HCI_BASE + BLE_ERR_CONN_SPVN_TMO);
    return "\"ok\":true,\"addr\":" + json::esc(addr);
}
