// The control channel (sim/PROTOCOL.md): what a test does to the device —
// taps, keys, GPS, sensors, the card, the clock — and what it reads back:
// state, the panel, the log. Also the console log every other module feeds.
#include <Arduino.h>
#include <TouchDrvGT911.hpp>

#include "ble_server.h"
#include "ble_shared.h"
#include "dash_config.h"
#include "ride_state.h"
#include "routes.h"
#include "settings.h"
#include "json.h"
#include "sim.h"
#include "sim_ble.h"
#include "sim_clock.h"

#include <arpa/inet.h>
#include <atomic>
#include <deque>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <regex>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

extern SharedRideState g_state;
std::string sensorCommand(const json::Value& r);   // sensors_sim.cpp
// -- sensors / radar / media inspection (sensors_sim.cpp, media_sim.cpp) --
std::string sensorStateCommand(const json::Value& r);
std::string mediaStateCommand(const json::Value& r);

// ---- the log --------------------------------------------------------------------------

namespace {
std::mutex g_logM;
std::condition_variable g_logCv;
std::deque<std::pair<uint64_t, std::string>> g_log;
uint64_t g_seq = 0;
std::vector<std::function<void(uint64_t, const std::string&)>> g_logCbs;
constexpr size_t kLogKeep = 50000;
}  // namespace

void sim::consoleLine(const std::string& line) {
    std::vector<std::function<void(uint64_t, const std::string&)>> cbs;
    uint64_t seq;
    {
        std::lock_guard<std::mutex> lk(g_logM);
        seq = ++g_seq;
        g_log.emplace_back(seq, line);
        if (g_log.size() > kLogKeep) g_log.pop_front();
        cbs = g_logCbs;
    }
    g_logCv.notify_all();
    if (!opts.quiet) { fprintf(stdout, "%s\n", line.c_str()); fflush(stdout); }
    for (auto& cb : cbs) cb(seq, line);
}

uint64_t sim::logSeq() {
    std::lock_guard<std::mutex> lk(g_logM);
    return g_seq;
}

std::vector<std::pair<uint64_t, std::string>> sim::logSince(uint64_t seq, size_t max) {
    std::lock_guard<std::mutex> lk(g_logM);
    std::vector<std::pair<uint64_t, std::string>> v;
    for (auto& e : g_log)
        if (e.first > seq && v.size() < max) v.push_back(e);
    return v;
}

bool sim::waitLog(const std::string& re, uint32_t timeoutMs, uint64_t since, std::string* hit, uint64_t* hitSeq) {
    std::regex rx(re);
    auto deadline = std::chrono::steady_clock::now() + sim_clock::realDuration(timeoutMs);
    std::unique_lock<std::mutex> lk(g_logM);
    for (;;) {
        for (auto& e : g_log) {
            if (e.first <= since) continue;
            since = e.first;
            if (std::regex_search(e.second, rx)) {
                if (hit) *hit = e.second;
                if (hitSeq) *hitSeq = e.first;
                return true;
            }
        }
        if (g_logCv.wait_until(lk, deadline) == std::cv_status::timeout) return false;
    }
}

void sim::onLogLine(std::function<void(uint64_t, const std::string&)> cb) {
    std::lock_guard<std::mutex> lk(g_logM);
    g_logCbs.push_back(std::move(cb));
}

void sim::feedConsole(const std::string& line) {
    std::string l = line + "\n";
    Serial.sim_feed((const uint8_t*)l.data(), l.size());
}

// ---- inputs ----------------------------------------------------------------------------

namespace {
std::mutex g_inM;
std::map<std::string, bool> g_buttons;
std::map<std::string, int> g_buttonReads;   // reads that saw it down, this press
int g_touchReads = 0;
bool g_touchDown = false;
int g_touchX = 0, g_touchY = 0;
std::atomic<bool> g_homePending{false};
}

void sim::setButton(const std::string& name, bool down) {
    std::lock_guard<std::mutex> lk(g_inM);
    g_buttons[name] = down;
}
bool sim::buttonDown(const std::string& name) {
    std::lock_guard<std::mutex> lk(g_inM);
    auto it = g_buttons.find(name);
    bool down = it != g_buttons.end() && it->second;
    if (down) ++g_buttonReads[name];
    return down;
}

namespace {
// Hold an input for `ms` of device time AND until the firmware has read it
// `reads` times: at a sped-up clock, drawing a frame still takes real CPU
// time, and a press shorter than one UI pass would never be seen.
void holdUntilRead(std::function<int()> readsSoFar, uint32_t ms, int reads) {
    uint32_t t0 = sim_clock::millis();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((sim_clock::millis() - t0 < ms || readsSoFar() < reads) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
}
}  // namespace
void sim::touch(int x, int y, bool down) {
    std::lock_guard<std::mutex> lk(g_inM);
    g_touchDown = down;
    g_touchX = x;
    g_touchY = y;
}
bool sim::touchState(int& x, int& y) {
    std::lock_guard<std::mutex> lk(g_inM);
    x = g_touchX;
    y = g_touchY;
    if (g_touchDown) ++g_touchReads;
    return g_touchDown;
}
void sim::homeKeyPress() { g_homePending = true; }

// The GT911 reports in the panel's portrait coordinates as ui_dashboard reads
// them; the home key fires its callback inside getPoint(), as the chip does.
uint8_t TouchDrvGT911::getPoint(int16_t& x, int16_t& y, uint8_t) {
    if (g_homePending.exchange(false) && homeCb_) homeCb_(homeUser_);
    int tx, ty;
    if (!sim::touchState(tx, ty)) return 0;
    x = (int16_t)tx;
    y = (int16_t)ty;
    return 1;
}
bool TouchDrvGT911::isPressed() {
    int x, y;
    return sim::touchState(x, y);
}

// ---- status ----------------------------------------------------------------------------

namespace {

std::string dashText() {
    std::string t(8192, 0);
    t.resize(dash_config::currentText(&t[0], t.size()));
    return t;
}

const char* screenName(int s) { return s == 0 ? "dash" : s == 1 ? "map" : "other"; }

// Turn-by-turn as the banner would show it: the next maneuver and the distance
// to it, from routes:: (the same calls the renderer makes).
std::string navJson() {
    char text[routes::MANEUVER_TEXT] = "";
    float m = -1;
    const bool next = routes::nextTurn(text, sizeof text, m);
    std::ostringstream o;
    o << "{\"route\":" << (routes::active() ? "true" : "false")
      << ",\"pending\":" << (routes::navPending() ? "true" : "false")
      << ",\"active\":" << (routes::navActive() ? "true" : "false")
      << ",\"maneuvers\":" << routes::maneuverCount()
      << ",\"next\":" << (next ? json::esc(text) : "null")
      << ",\"next_m\":" << (next ? m : -1)
      << ",\"remaining_km\":" << routes::remainingKm() << "}";
    return o.str();
}

// The card's size and usage, as SD.cardSize/totalBytes/usedBytes report them.
std::string sdJson(bool recount) {
    const sim::CardUsage u = sim::sdUsage(recount);
    std::ostringstream o;
    o << "{\"present\":" << (sim::sdPresent() ? "true" : "false") << ",\"card_bytes\":" << u.cardBytes
      << ",\"total_bytes\":" << u.totalBytes << ",\"used_bytes\":" << u.usedBytes
      << ",\"free_bytes\":" << (u.totalBytes - u.usedBytes) << ",\"cluster_bytes\":" << u.clusterBytes << "}";
    return o.str();
}

std::string statusJson() {
    RideState s = g_state.snapshot();
    ble_server::LiveView v;
    ble_shared::readLiveView(v);
    std::ostringstream o;
    o.precision(10);
    o << "{\"uptime_ms\":" << millis() << ",\"wall_unix\":" << (long)time(nullptr)
      << ",\"speed\":" << sim_clock::factor() << ",\"paints\":" << sim::paintCount()
      << ",\"scrubs\":" << sim::scrubCount() << ",\"clears\":" << sim::clearCount()
      << ",\"screen\":\"" << screenName(v.screen) << "\",\"page\":" << (v.page == 0xFF ? -1 : (int)v.page)
      << ",\"page_count\":" << (int)v.pageCount << ",\"phone_connected\":" << (ble_server::isPhoneConnected() ? "true" : "false")
      << ",\"phone_paired\":" << (ble_server::phonePaired() ? "true" : "false")
      << ",\"phone_name\":" << json::esc(ble_server::pairedPhoneName())
      << ",\"ride\":{\"recording\":" << (s.recording ? "true" : "false") << ",\"paused\":" << (s.ridePaused ? "true" : "false")
      << ",\"distance_m\":" << s.distanceM << ",\"elapsed_s\":" << s.elapsedS << ",\"moving_s\":" << s.movingS
      << ",\"speed_kmh\":" << s.speedKmh << ",\"gps_fix\":" << (s.gpsFix ? "true" : "false")
      << ",\"lat\":" << s.latitude << ",\"lon\":" << s.longitude << ",\"sats\":" << (int)s.satellites
      << ",\"time_valid\":" << (s.timeValid ? "true" : "false")
      << ",\"card_full\":" << (s.cardFull ? "true" : "false")
      << ",\"hr\":" << (s.hrConnected ? (int)s.heartRateBpm : -1)
      << ",\"power\":" << (s.powerConnected && s.powerW != 0xFFFF ? (int)s.powerW : -1)
      << ",\"cadence\":" << (s.cadenceConnected && s.cadenceRpm != 0xFF ? (int)s.cadenceRpm : -1)
      << ",\"battery\":" << (int)s.batteryPercent << ",\"route_active\":" << (s.routeActive ? "true" : "false")
      << ",\"radar_connected\":" << (s.radar.connected ? "true" : "false") << ",\"radar_targets\":" << (int)s.radar.count
      << "},\"reset_reason\":" << sim::resetReason()
      << ",\"settings\":{\"ftp\":" << settings::ftpWatts() << ",\"tz_min\":" << settings::tzMinutes()
      << ",\"miles\":" << (settings::useMiles() ? "true" : "false") << ",\"clock24h\":" << (settings::clock24h() ? "true" : "false")
      << ",\"show_offline\":" << (settings::showOffline() ? "true" : "false") << ",\"auto_pause_s\":" << settings::autoPauseSec()
      << ",\"wheel_mm\":" << settings::wheelCircMm() << ",\"backlight\":" << settings::backlight()
      << ",\"map_layers\":" << settings::mapLayers() << "}"
      << ",\"nav\":" << navJson()
      << ",\"dash\":" << json::esc(dashText())
      << ",\"sd_present\":" << (sim::sdPresent() ? "true" : "false")
      << ",\"sd\":" << sdJson(false)
      << ",\"gps\":" << sim::gpsStatusJson()
      << ",\"ble\":" << sim::bleStatusJson() << "}";
    return o.str();
}

// The tiles (or .poi files) on the card, as the device lists them to a phone:
// the TileList answer's own walk and version records (ble_shared::lockTileList,
// link.cpp), so a test sees what any phone asking now would see.
std::string tilesJson(bool poi) {
    ble_shared::TileListing tl;
    if (!ble_shared::lockTileList(poi, true, tl)) return "null";
    std::ostringstream o;
    o << "[";
    for (int i = 0; i < tl.n; ++i) {
        o << (i ? "," : "") << "{\"id\":" << json::esc(tl.ids[i])
          << ",\"version\":" << json::esc(tl.vers ? tl.vers[i] : "") << "}";
    }
    o << "]";
    ble_shared::unlockTileList();
    return o.str();
}

std::string handle(const json::Value& r) {
    const std::string cmd = r.str("cmd");
    auto ok = [](const std::string& extra = "") { return std::string("\"ok\":true") + (extra.empty() ? "" : "," + extra); };
    if (cmd == "status") return ok("\"status\":" + statusJson());
    if (cmd == "tiles") {
        const bool poi = r.boolean("poi", false);
        const std::string list = tilesJson(poi);
        if (list == "null") return "\"ok\":false,\"error\":\"no memory for the tile list\"";
        return ok("\"tiles\":" + list);
    }
    if (cmd == "console") {
        uint64_t since = sim::logSeq();
        sim::feedConsole(r.str("line"));
        sim_clock::sleepMs((uint32_t)r.num("wait_ms", 500));
        std::string out = "[";
        bool first = true;
        for (auto& e : sim::logSince(since)) { out += (first ? "" : ",") + json::esc(e.second); first = false; }
        return ok("\"lines\":" + out + "]");
    }
    if (cmd == "screenshot") {
        std::string path = r.str("path", sim::opts.dir + "/screen.png");
        return sim::writePng(path) ? ok("\"path\":" + json::esc(path)) : "\"ok\":false,\"error\":\"write failed\"";
    }
    if (cmd == "tap") {
        int x = (int)r.num("x"), y = (int)r.num("y");
        { std::lock_guard<std::mutex> lk(g_inM); g_touchReads = 0; }
        sim::touch(x, y, true);
        holdUntilRead([] { std::lock_guard<std::mutex> lk(g_inM); return g_touchReads; },
                      (uint32_t)r.num("ms", 120), 2);
        sim::touch(x, y, false);
        return ok();
    }
    if (cmd == "touch") { sim::touch((int)r.num("x"), (int)r.num("y"), r.boolean("down", true)); return ok(); }
    if (cmd == "button") {
        std::string name = r.str("name", "boot");
        if (name == "home") { sim::homeKeyPress(); return ok(); }
        { std::lock_guard<std::mutex> lk(g_inM); g_buttonReads[name] = 0; }
        sim::setButton(name, true);
        holdUntilRead([name] { std::lock_guard<std::mutex> lk(g_inM); return g_buttonReads[name]; },
                      (uint32_t)r.num("ms", 150), 1);
        sim::setButton(name, false);
        return ok();
    }
    if (cmd == "gps") {
        if (r.has("track")) {
            std::vector<std::pair<double, double>> pts;
            for (auto& p : r["track"].a) if (p.a.size() >= 2) pts.push_back({p.a[0].n, p.a[1].n});
            sim::gpsTrack(pts, r.num("speed_kmh", 25), r.boolean("loop", false));
        } else {
            sim::gpsSet(r.num("lat"), r.num("lon"), r.num("alt", 50), r.num("speed_kmh", 0), r.num("course", 0),
                        r.boolean("fix", true), (int)r.num("sats", 10), r.boolean("time", true));
        }
        return ok();
    }
    if (cmd == "gps_silent") { sim::gpsSilent(r.boolean("on", true)); return ok(); }
    if (cmd == "battery") { sim::setBattery((int)r.num("pct", 100), r.boolean("charging", false)); return ok(); }
    if (cmd == "wallclock") { sim_clock::setWallMicros((int64_t)(r.num("unix") * 1e6)); return ok(); }
    if (cmd == "sd") {
        if (r.has("latency_ms")) sim::setSdLatency((int)r.num("latency_ms"));
        else if (r.has("size_mb")) sim::setSdSizeMb((uint64_t)r.num("size_mb"), (uint32_t)r.num("cluster_kb", 0));
        else if (!r.has("usage")) sim::setSdPresent(r.boolean("present", true));
        return ok("\"sd\":" + sdJson(true));
    }
    if (cmd == "usb") { sim::setUsbHost(r.boolean("host", true)); return ok(); }
    if (cmd == "speed") { sim_clock::setFactor(r.num("factor", 1)); return ok(); }
    if (cmd == "ble_drop") { simble::dropPhone((int)r.num("reason", 0x08)); return ok(); }
    if (cmd == "log") {
        std::string out = "[";
        bool first = true;
        uint64_t last = (uint64_t)r.num("since", 0);
        for (auto& e : sim::logSince(last, (size_t)r.num("max", 5000))) {
            out += (first ? "" : ",") + std::string("[") + std::to_string(e.first) + "," + json::esc(e.second) + "]";
            first = false;
            last = e.first;
        }
        return ok("\"lines\":" + out + "],\"seq\":" + std::to_string(last));
    }
    if (cmd == "wait_log") {
        std::string hit;
        uint64_t seq = 0;
        uint64_t since = r.has("since") ? (uint64_t)r.num("since") : sim::logSeq();
        if (sim::waitLog(r.str("pattern"), (uint32_t)r.num("timeout_ms", 5000), since, &hit, &seq))
            return ok("\"line\":" + json::esc(hit) + ",\"seq\":" + std::to_string(seq));
        return "\"ok\":false,\"error\":\"timeout\"";
    }
    if (cmd == "seq") return ok("\"seq\":" + std::to_string(sim::logSeq()));
    if (cmd == "reboot") { std::thread([] { sim_clock::sleepMs(50); sim::reboot("control"); }).detach(); return ok(); }
    if (cmd == "quit") { std::thread([] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); _exit(0); }).detach(); return ok(); }
    if (cmd == "sensor" || cmd == "sensor_remove") return sensorCommand(r);
    // -- sensors / radar / media inspection --
    if (cmd == "sensor_state") return sensorStateCommand(r);
    if (cmd == "media_state") return mediaStateCommand(r);
    // ---- power and robustness ----------------------------------------------------------
    if (cmd == "reset") {   // a crash, a watchdog, a brownout: reset with that reason
        const int reason = sim::resetReasonByName(r.str("reason", "panic"));
        if (reason < 0) return "\"ok\":false,\"error\":\"unknown reset reason\"";
        std::string why = "control reset: " + r.str("reason", "panic");
        std::thread([reason, why] { sim_clock::sleepMs(50); sim::reboot(why.c_str(), reason); }).detach();
        return ok();
    }
    if (cmd == "gps_time") { sim::gpsSetUtc((int64_t)r.num("unix")); return ok(); }
    if (cmd == "gps_raw") { sim::gpsRaw(r.str("data")); return ok(); }
    if (cmd == "gps_paced") { sim::gpsPaced(r.boolean("on", true)); return ok(); }
    return "\"ok\":false,\"error\":" + json::esc("unknown cmd '" + cmd + "'");
}

void serveClient(int fd) {
    pthread_setname_np("control");
    std::string buf;
    char tmp[4096];
    std::mutex wm;
    auto writeLine = [&](const std::string& s) {
        std::lock_guard<std::mutex> lk(wm);
        std::string l = s + "\n";
        const char* p = l.data();
        size_t left = l.size();
        while (left) {
            ssize_t n = ::send(fd, p, left, 0);
            if (n <= 0) return;
            p += n;
            left -= (size_t)n;
        }
    };
    auto streaming = std::make_shared<std::atomic<bool>>(false);
    auto alive = std::make_shared<std::atomic<bool>>(true);
    for (;;) {
        ssize_t n = recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) break;
        buf.append(tmp, (size_t)n);
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (line.empty()) continue;
            json::Value r;
            std::string id = "null", body;
            if (!json::parse(line, r) || r.type != json::Value::Object) {
                body = "\"ok\":false,\"error\":\"bad json\"";
            } else {
                if (r.has("id")) id = r["id"].type == json::Value::String ? json::esc(r["id"].s)
                                                                         : std::to_string((long long)r["id"].n);
                if (r.str("cmd") == "subscribe_log") {
                    bool on = r.boolean("on", true);
                    if (on && !streaming->exchange(true)) {
                        auto st = streaming;
                        auto al = alive;
                        sim::onLogLine([st, al, writeLine](uint64_t seq, const std::string& l) {
                            if (*st && *al) writeLine("{\"event\":\"log\",\"seq\":" + std::to_string(seq) + ",\"line\":" + json::esc(l) + "}");
                        });
                    } else if (!on) {
                        *streaming = false;
                    }
                    body = "\"ok\":true";
                } else {
                    body = handle(r);
                }
            }
            writeLine("{\"id\":" + id + "," + body + "}");
        }
    }
    *alive = false;
    close(fd);
}

}  // namespace

void sim::controlBegin() {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)opts.ctlPort);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lfd, (sockaddr*)&a, sizeof a) != 0 || listen(lfd, 4) != 0) {
        fprintf(stderr, "[sim] control: can't listen on %d\n", opts.ctlPort);
        exit(2);
    }
    std::thread([lfd] {
        for (;;) {
            int fd = accept(lfd, nullptr, nullptr);
            if (fd < 0) continue;
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
            std::thread(serveClient, fd).detach();
        }
    }).detach();
}
