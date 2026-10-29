// Virtual BLE sensors for the firmware's own sensor code (src/ble_sensors.cpp):
// each advertises its standard service and sends standard measurement packets
// — Heart Rate 0x2A37, Cycling Power 0x2A63 (with crank data), CSC 0x2A5B,
// a Varia radar — so pairing, decoding, drop-outs and reconnects are the
// firmware's real paths. Driven by the control channel's `sensor` command.
#include <Arduino.h>

#include "ble_sensors.h"
#include "json.h"
#include "ride_state.h"
#include "settings.h"
#include "sim.h"
#include "speed_source.h"
#include "sim_ble.h"
#include "sim_clock.h"

#include <cmath>
#include <sstream>
#include <thread>

namespace {

struct VSensor {
    std::string kind;
    std::shared_ptr<simble::SimPeripheral> per;
    NimBLEUUID svc, chr;
    double bpm = 0, watts = 0, cadence = 0, speedKmh = 0;
    std::vector<std::pair<int, int>> radar;   // (track id, metres behind)
    bool sending = true;
    // Packet shape, for exercising the decoders' optional fields.
    bool hrWide = false;     // HR: 16-bit value format (flags bit 0)
    int hrContact = -1;      // HR: -1 contact detection unsupported, 0 no contact, 1 contact
    int balance = -1;        // power: pedal power balance in % (flags bit 0), -1 absent
    bool torque = false;     // power: accumulated torque present (flags bit 2)
    bool powerWheel = false; // power: wheel revolution data present (flags bit 4)
    bool crank = true;       // power: crank revolution data present (flags bit 5)
    double torqueAcc = 0;
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
                              const std::string& make, const std::string& model, int feature) {
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
    if (kind == "csc" || kind == "speed" || kind == "cadence") {
        // CSC Feature: wheel (speed), crank (cadence), both (csc). `feature`
        // overrides it; -1 leaves the characteristic out (a sensor that gets
        // the mandatory Feature wrong: the firmware learns from the packets).
        uint16_t feat = kind == "speed" ? 0x0001 : kind == "cadence" ? 0x0002 : 0x0003;
        if (feature >= 0) feat = (uint16_t)feature;
        if (feature != -1)
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
        uint8_t flags = s.hrWide ? 0x01 : 0x00;
        if (s.hrContact >= 0) flags |= 0x04 | (s.hrContact ? 0x02 : 0x00);
        const uint16_t bpm = (uint16_t)std::min(s.hrWide ? 65535.0 : 255.0, s.bpm);
        pkt = std::string{(char)flags};
        pkt += s.hrWide ? u16(bpm) : std::string{(char)(uint8_t)bpm};
    } else if (s.kind == "power") {
        revs(s.crankRevs, s.lastCrankT, s.cadence, dt, nowS);
        double wheelRpm = s.speedKmh / 3.6 / (s.wheelMm / 1000.0) * 60.0;
        if (s.powerWheel) revs(s.wheelRevs, s.lastWheelT, wheelRpm, dt, nowS);
        s.torqueAcc += s.cadence > 0 ? s.watts / (s.cadence * 2 * M_PI / 60.0) * dt * 32 : 0;
        uint16_t flags = (s.balance >= 0 ? 0x0003 : 0) | (s.torque ? 0x0004 : 0) |
                         (s.powerWheel ? 0x0010 : 0) | (s.crank ? 0x0020 : 0);
        pkt = u16(flags) + u16((uint16_t)(int16_t)lround(s.watts));
        if (s.balance >= 0) pkt += (char)(uint8_t)std::min(255, s.balance * 2);   // 1/2 %, left
        if (s.torque) pkt += u16((uint16_t)((uint32_t)s.torqueAcc & 0xFFFF));
        // Cycling Power's wheel event time is 1/2048 s (CSC's is 1/1024).
        if (s.powerWheel) pkt += u32((uint32_t)s.wheelRevs) + u16((uint16_t)(s.lastWheelT * 2));
        if (s.crank) pkt += u16((uint16_t)((uint32_t)s.crankRevs & 0xFFFF)) + u16(s.lastCrankT);
    } else if (s.kind == "csc" || s.kind == "speed" || s.kind == "cadence") {
        pkt = std::string{(char)0};
        if (s.kind != "cadence") {
            double wheelRpm = s.speedKmh / 3.6 / (s.wheelMm / 1000.0) * 60.0;
            revs(s.wheelRevs, s.lastWheelT, wheelRpm, dt, nowS);
            pkt[0] |= 0x01;
            pkt += u32((uint32_t)(uint64_t)s.wheelRevs) + u16(s.lastWheelT);
        }
        if (s.kind != "speed") {
            revs(s.crankRevs, s.lastCrankT, s.cadence, dt, nowS);
            pkt[0] |= 0x02;
            pkt += u16((uint16_t)((uint32_t)s.crankRevs & 0xFFFF)) + u16(s.lastCrankT);
        }
    } else if (s.kind == "radar") {
        // Varia legacy format: a header byte (frame counter in the high
        // nibble, 0x2 in the low one), then 3 bytes per track — id with bit 7
        // set, metres behind, closing speed. Six tracks fit a 20-byte
        // notification; the rest follow in continuation packets whose header
        // is the previous one + 2, as the firmware's radarIngest expects.
        s.radarSeq = (uint8_t)(((s.radarSeq >> 4) + 1) << 4 | 0x02);
        uint8_t hdr = s.radarSeq;
        size_t i = 0;
        do {
            pkt = std::string{(char)hdr};
            for (int n = 0; n < 6 && i < s.radar.size(); ++n, ++i) {
                pkt += (char)(0x80 | (s.radar[i].first & 0x7f));
                pkt += (char)(uint8_t)std::max(0, std::min(255, s.radar[i].second));
                pkt += (char)40;   // closing speed, km/h
            }
            simble::peripheralNotify(s.per, s.svc, s.chr, (const uint8_t*)pkt.data(), pkt.size());
            hdr = (uint8_t)(hdr + 2);
        } while (i < s.radar.size());
        return;
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
    if (kind == "cadence") return "c0:ff:ee:00:00:06";
    if (kind == "hr") return "c0:ff:ee:00:00:01";
    if (kind == "power") return "c0:ff:ee:00:00:02";
    if (kind == "csc") return "c0:ff:ee:00:00:03";
    if (kind == "speed") return "c0:ff:ee:00:00:04";
    return "c0:ff:ee:00:00:05";
}

std::string defaultName(const std::string& kind) {
    if (kind == "cadence") return "CAD1-Sim";
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
        s = make(kind, addr, r.str("name", defaultName(kind)), r.str("make", "Sim"), r.str("model", defaultName(kind)),
                 r.has("feature") ? (r["feature"].type == json::Value::Null ? -1 : (int)r.num("feature")) : -2);
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
        if (r.has("adv_uuids")) {   // what the advert lists, in place of the measurement service
            s->per->advUuids.clear();
            for (auto& u : r["adv_uuids"].a)
                if (u.type == json::Value::String) s->per->advUuids.push_back(NimBLEUUID(u.s));
        }
    }
    if (r.has("bpm")) s->bpm = r.num("bpm");
    if (r.has("watts")) s->watts = r.num("watts");
    if (r.has("cadence")) s->cadence = r.num("cadence");
    if (r.has("speed_kmh")) s->speedKmh = r.num("speed_kmh");
    if (r.has("sending")) s->sending = r.boolean("sending");
    if (r.has("targets")) {
        // [metres, …] (ids 1, 2, … by position) or [[id, metres], …].
        s->radar.clear();
        int id = 1;
        for (auto& t : r["targets"].a) {
            if (t.type == json::Value::Array && t.a.size() >= 2) s->radar.push_back({(int)t.a[0].n, (int)t.a[1].n});
            else s->radar.push_back({id, (int)t.n});
            ++id;
        }
    }
    // -- packet shape and counters (sim/PROTOCOL.md, `sensor`) --
    if (r.has("wheel_mm")) s->wheelMm = (uint32_t)r.num("wheel_mm");
    if (r.has("wheel_revs")) s->wheelRevs = r.num("wheel_revs");
    if (r.has("crank_revs")) s->crankRevs = r.num("crank_revs");
    if (r.boolean("reset")) { s->wheelRevs = s->crankRevs = 0; s->lastWheelT = s->lastCrankT = 0; s->torqueAcc = 0; }
    if (r.has("hr_wide")) s->hrWide = r.boolean("hr_wide");
    if (r.has("contact")) s->hrContact = r["contact"].type == json::Value::Null ? -1 : (r.boolean("contact") ? 1 : 0);
    if (r.has("balance")) s->balance = r["balance"].type == json::Value::Null ? -1 : (int)r.num("balance");
    if (r.has("torque")) s->torque = r.boolean("torque");
    if (r.has("power_wheel")) s->powerWheel = r.boolean("power_wheel");
    if (r.has("crank")) s->crank = r.boolean("crank");
    if (r.boolean("drop")) simble::peripheralGone(s->per, BLE_HS_ERR_HCI_BASE + BLE_ERR_CONN_SPVN_TMO);
    return "\"ok\":true,\"addr\":" + json::esc(addr);
}

// ---- `sensor_state`: what the firmware made of its sensors ---------------------------
// The RideState fields the sensor code writes (values, freshness, the speed
// arbitration, the radar tracks) and each kind's pairing and link, as the
// `sensors` console command lists them. Ages are device milliseconds since the
// last reading (-1: never).
std::string sensorStateCommand(const json::Value&) {
    RideState s = g_state.snapshot();
    const uint32_t now = millis();
    auto age = [now](uint32_t ms) { return ms ? (long)(uint32_t)(now - ms) : -1L; };
    std::ostringstream o;
    o.precision(10);
    o << "\"ok\":true,\"state\":{"
      << "\"hr_connected\":" << (s.hrConnected ? "true" : "false")
      << ",\"hr_bpm\":" << (s.heartRateBpm == 0xFF ? -1 : (int)s.heartRateBpm) << ",\"hr_age_ms\":" << age(s.hrMs)
      << ",\"power_connected\":" << (s.powerConnected ? "true" : "false")
      << ",\"power_w\":" << (s.powerW == 0xFFFF ? -1 : (int)s.powerW)
      << ",\"power3s_w\":" << (s.power3sW == 0xFFFF ? -1 : (int)s.power3sW) << ",\"power_age_ms\":" << age(s.powerMs)
      << ",\"cadence_connected\":" << (s.cadenceConnected ? "true" : "false")
      << ",\"cadence_rpm\":" << (s.cadenceRpm == 0xFF ? -1 : (int)s.cadenceRpm) << ",\"cadence_age_ms\":" << age(s.cadenceMs)
      << ",\"speed_sensor_connected\":" << (s.speedSensorConnected ? "true" : "false")
      << ",\"wheel_speed_kmh\":" << s.wheelSpeedKmh << ",\"wheel_dist_m\":" << s.wheelDistM
      << ",\"wheel_age_ms\":" << age(s.wheelDataMs) << ",\"wheel_move_age_ms\":" << age(s.wheelMoveMs)
      << ",\"speed_kmh\":" << s.speedKmh << ",\"speed_source\":\"" << speed_source::name(s.speedSource) << "\""
      << ",\"wheel_mm\":" << settings::wheelCircMm()
      << ",\"radar\":{\"connected\":" << (s.radar.connected ? "true" : "false")
      << ",\"live\":" << (s.radar.live ? "true" : "false") << ",\"targets\":[";
    for (int i = 0; i < s.radar.count; ++i)
        o << (i ? "," : "") << "{\"id\":" << (int)s.radar.targets[i].id << ",\"m\":" << (int)s.radar.targets[i].distanceM << "}";
    o << "]}},\"links\":[";
    ble_sensors::Link ls[ble_sensors::KIND_COUNT];
    ble_sensors::links(ls);
    for (int k = 0; k < ble_sensors::KIND_COUNT; ++k) {
        o << (k ? "," : "") << "{\"kind\":" << json::esc(ls[k].kind) << ",\"paired\":" << json::esc(ls[k].pairedAddr)
          << ",\"paired_all\":[";
        for (int i = 0; i < settings::sensorAddrCount(k); ++i)
            o << (i ? "," : "") << json::esc(settings::sensorAddrAt(k, i));
        o << "],\"live\":" << json::esc(ls[k].liveAddr) << ",\"name\":" << json::esc(ls[k].name)
          << ",\"connected\":" << (ls[k].connected ? "true" : "false") << "}";
    }
    o << "]";
    return o.str();
}
