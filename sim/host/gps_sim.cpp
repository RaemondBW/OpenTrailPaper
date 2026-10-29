// An L76K GPS module on Serial2, for src/gps_service.cpp as it is: it obeys
// the PCAS commands the firmware sends (output on/off, the version query, baud
// changes, save), keeps its baud across sim restarts (the module's backup
// supply), and streams GGA/RMC/GSA/GSV at 1 Hz for the position a test sets or
// a track it plays. A UART at the wrong baud reads garbage, as on the board.
#include <Arduino.h>

#include "sim.h"
#include "sim_clock.h"

#include <chrono>
#include <cmath>
#include <mutex>
#include <sstream>
#include <thread>

namespace {

std::mutex m;
uint32_t moduleBaud = 9600;
bool output = true;
bool silent = false;   // unplugged / browned out: sends and hears nothing
bool fix = false;
bool timeKnown = true;   // without a fix: does the module report UTC (a cold one indoors doesn't)
int sats = 0;
double lat = 0, lon = 0, alt = 50, speedKmh = 0, course = 0;
std::vector<std::pair<double, double>> track;
size_t trackIdx = 0;
bool trackLoop = false;
std::string cmdLine;
int64_t utcBaseUs = 0;   // GPS time at sim boot (real UTC)
uint32_t sentences = 0;
bool paced = false;      // deliver sentence by sentence at the UART's byte rate

std::string baudPath() { return sim::opts.dir + "/gps_baud.txt"; }
std::string clockPath() { return sim::opts.dir + "/gps_clock.txt"; }

uint8_t checksum(const std::string& body) {
    uint8_t c = 0;
    for (char ch : body) c ^= (uint8_t)ch;
    return c;
}

std::string sentence(const std::string& body) {
    char ck[8];
    snprintf(ck, sizeof ck, "*%02X\r\n", checksum(body));
    return "$" + body + ck;
}

// Bytes from the module: what the firmware's UART makes of them.
void deliver(const std::string& s) {
    if (Serial2.baudRate() == moduleBaud) {
        Serial2.sim_feed((const uint8_t*)s.data(), s.size());
        return;
    }
    std::string junk(s.size() * Serial2.baudRate() / std::max<uint32_t>(moduleBaud, 1) + 1, 0);
    for (auto& c : junk) c = (char)(esp_random() & 0x7F);
    Serial2.sim_feed((const uint8_t*)junk.data(), junk.size());
}

void command(const std::string& line) {
    // "$PCASnn,args*CK"
    if (line.size() < 7 || line.compare(0, 5, "$PCAS") != 0) return;
    int id = atoi(line.c_str() + 5);
    const char* args = strchr(line.c_str(), ',');
    int a0 = args ? atoi(args + 1) : -1;
    switch (id) {
        case 0: {   // save
            if (FILE* f = fopen(baudPath().c_str(), "w")) { fprintf(f, "%u\n", moduleBaud); fclose(f); }
            break;
        }
        case 1: {   // baud
            static const uint32_t bauds[] = {4800, 9600, 19200, 38400, 57600, 115200};
            if (a0 >= 0 && a0 < 6) moduleBaud = bauds[a0];
            break;
        }
        case 3: output = a0 != 0 || (args && strstr(args, ",1")); break;   // all-zero = off
        case 6: deliver(sentence("GPTXT,01,01,02,MA=CASIC")); deliver(sentence("GPTXT,01,01,02,IC=AT6558-5N-32-1C510800")); break;
        case 10: fix = false; break;   // restart: the fix is reacquired next second
        default: break;
    }
}

std::string ddmm(double deg, bool isLat) {
    double a = fabs(deg);
    int d = (int)a;
    double minutes = (a - d) * 60.0;
    char b[32];
    if (isLat) snprintf(b, sizeof b, "%02d%08.5f,%c", d, minutes, deg >= 0 ? 'N' : 'S');
    else snprintf(b, sizeof b, "%03d%08.5f,%c", d, minutes, deg >= 0 ? 'E' : 'W');
    return b;
}

double haversineM(double la1, double lo1, double la2, double lo2) {
    const double R = 6371000.0, r = M_PI / 180;
    double dla = (la2 - la1) * r, dlo = (lo2 - lo1) * r;
    double h = sin(dla / 2) * sin(dla / 2) + cos(la1 * r) * cos(la2 * r) * sin(dlo / 2) * sin(dlo / 2);
    return 2 * R * asin(std::min(1.0, sqrt(h)));
}

double bearing(double la1, double lo1, double la2, double lo2) {
    const double r = M_PI / 180;
    double y = sin((lo2 - lo1) * r) * cos(la2 * r);
    double x = cos(la1 * r) * sin(la2 * r) - sin(la1 * r) * cos(la2 * r) * cos((lo2 - lo1) * r);
    return fmod(atan2(y, x) / r + 360, 360);
}

// One second of motion.
void advance() {
    double metres = speedKmh / 3.6;
    if (!track.empty()) {
        while (metres > 0 && trackIdx + 1 < track.size()) {
            auto [la2, lo2] = track[trackIdx + 1];
            double d = haversineM(lat, lon, la2, lo2);
            course = bearing(lat, lon, la2, lo2);
            if (d <= metres) {
                lat = la2; lon = lo2; metres -= d; ++trackIdx;
                if (trackIdx + 1 >= track.size() && trackLoop) { trackIdx = 0; lat = track[0].first; lon = track[0].second; }
            } else {
                double f = metres / d;
                lat += (la2 - lat) * f;
                lon += (lo2 - lon) * f;
                metres = 0;
            }
        }
        if (trackIdx + 1 >= track.size() && !trackLoop) speedKmh = 0;
        return;
    }
    if (metres <= 0) return;
    const double r = M_PI / 180;
    lat += metres * cos(course * r) / 111320.0;
    lon += metres * sin(course * r) / (111320.0 * std::max(0.01, cos(lat * r)));
}

void tick() {
    std::string out;
    std::vector<std::string> parts;
    bool pace = false;
    {
        std::lock_guard<std::mutex> lk(m);
        if (fix) advance();
        if (!output || silent) return;
        int64_t us = utcBaseUs + (int64_t)sim_clock::micros64();
        time_t t = (time_t)(us / 1000000);
        struct tm g;
        gmtime_r(&t, &g);
        char hms[16], date[8];
        snprintf(hms, sizeof hms, "%02d%02d%02d.00", g.tm_hour, g.tm_min, g.tm_sec);
        snprintf(date, sizeof date, "%02d%02d%02d", g.tm_mday, g.tm_mon + 1, g.tm_year % 100);
        std::ostringstream gga, rmc, gsa;
        if (fix) {
            gga << "GNGGA," << hms << "," << ddmm(lat, true) << "," << ddmm(lon, false) << ",1," << sats
                << ",0.9," << alt << ",M,0.0,M,,";
            char sp[16], co[16];
            snprintf(sp, sizeof sp, "%.2f", speedKmh / 1.852);
            snprintf(co, sizeof co, "%.1f", course);
            rmc << "GNRMC," << hms << ",A," << ddmm(lat, true) << "," << ddmm(lon, false) << "," << sp << ","
                << co << "," << date << ",,,A,V";
            gsa << "GNGSA,A,3,01,02,03,04,05,06,07,08,,,,,1.5,0.9,1.2,1";
        } else {
            if (!timeKnown) hms[0] = date[0] = 0;
            gga << "GNGGA," << hms << ",,,,,0," << sats << ",25.5,,,,,,";
            rmc << "GNRMC," << hms << ",V,,,,,,," << date << ",,,N,V";
            gsa << "GNGSA,A,1,,,,,,,,,,,,,25.5,25.5,25.5,1";
        }
        parts = {sentence(gga.str()), sentence(rmc.str()), sentence(gsa.str())};
        if (++sentences % 5 == 0) {
            std::ostringstream gsv;
            gsv << "GPGSV,1,1," << sats << ",01,45,090,38,02,30,180,35,03,60,270,40,04,20,045,30,0";
            parts.push_back(sentence(gsv.str()));
        }
        pace = paced;
    }
    if (!pace) {
        for (auto& p : parts) out += p;
        deliver(out);
        return;
    }
    // As the wire has them: each sentence lands after its bytes have crossed
    // the UART (10 bits a byte), so the firmware can parse GGA before RMC exists.
    for (auto& p : parts) {
        sim_clock::sleepMs(std::max<uint32_t>(1, (uint32_t)(p.size() * 10000 / std::max<uint32_t>(moduleBaud, 1))));
        deliver(p);
    }
}

}  // namespace

void sim::gpsBegin() {
    {
        std::lock_guard<std::mutex> lk(m);
        if (FILE* f = fopen(baudPath().c_str(), "r")) {
            unsigned b = 0;
            if (fscanf(f, "%u", &b) == 1 && b) moduleBaud = b;
            fclose(f);
        }
        timeKnown = !sim::opts.gpsCold;
        utcBaseUs = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
        // The module's RTC runs on its backup supply across a reset of the
        // ESP32: carry its clock on from where the last boot left it (plus
        // the real time the reset took), so GPS time never steps backwards
        // after a sped-up clock or a gps_time.
        if (FILE* f = fopen(clockPath().c_str(), "r")) {
            long long utcUs = 0, atRealUs = 0;
            if (fscanf(f, "%lld %lld", &utcUs, &atRealUs) == 2 && utcUs > 0) utcBaseUs = utcUs + (utcBaseUs - atRealUs);
            fclose(f);
        }
    }
    // What the firmware writes to the module.
    Serial2.sink = [](const uint8_t* b, size_t n) {
        std::lock_guard<std::mutex> lk(m);
        if (silent || Serial2.baudRate() != moduleBaud) return;   // garbage to the module
        for (size_t i = 0; i < n; ++i) {
            char c = (char)b[i];
            if (c == '$') cmdLine.clear();
            if (c == '\n') { command(cmdLine); cmdLine.clear(); continue; }
            if (c != '\r') cmdLine += c;
        }
    };
    std::thread([] {
        pthread_setname_np("gps_module");
        // 1 Hz on a schedule, not "sleep a second after each tick": the tick's
        // own time (more of it at a sped-up clock) would otherwise stretch
        // every second, and a rider at 36 km/h covered 1 km in 103 s.
        uint64_t next = sim_clock::micros64() / 1000 + 1000;
        for (;;) {
            const uint64_t now = sim_clock::micros64() / 1000;
            if (next > now) sim_clock::sleepMs((uint32_t)(next - now));
            tick();
            next += 1000;
            if (now > next + 5000) next = now + 1000;   // the clock jumped: don't race to catch up
        }
    }).detach();
}

void sim::gpsSet(double la, double lo, double al, double sp, double co, bool f, int s, bool t) {
    std::lock_guard<std::mutex> lk(m);
    timeKnown = t;
    lat = la; lon = lo; alt = al; speedKmh = sp; course = co; fix = f; sats = s;
    track.clear();
}

void sim::gpsTrack(const std::vector<std::pair<double, double>>& pts, double sp, bool loop) {
    std::lock_guard<std::mutex> lk(m);
    track = pts;
    trackIdx = 0;
    trackLoop = loop;
    if (!pts.empty()) { lat = pts[0].first; lon = pts[0].second; }
    speedKmh = sp;
    fix = true;
    if (sats < 6) sats = 10;
}

void sim::gpsSilent(bool on) {
    std::lock_guard<std::mutex> lk(m);
    silent = on;
}

std::string sim::gpsStatusJson() {
    std::lock_guard<std::mutex> lk(m);
    std::ostringstream o;
    o.precision(10);
    o << "{\"module_baud\":" << moduleBaud << ",\"uart_baud\":" << Serial2.baudRate() << ",\"output\":"
      << (output ? "true" : "false") << ",\"silent\":" << (silent ? "true" : "false") << ",\"fix\":" << (fix ? "true" : "false") << ",\"lat\":" << lat
      << ",\"lon\":" << lon << ",\"speed_kmh\":" << speedKmh << ",\"track_left\":"
      << (track.empty() ? 0 : (int)(track.size() - trackIdx - 1)) << "}";
    return o.str();
}

// ---- power and robustness: the module's clock, raw bytes, wire pacing -------------------

void sim::gpsSetUtc(int64_t unix) {
    std::lock_guard<std::mutex> lk(m);
    utcBaseUs = unix * 1000000 - (int64_t)sim_clock::micros64();
}

void sim::gpsRaw(const std::string& bytes) { deliver(bytes); }

void sim::gpsPaced(bool on) {
    std::lock_guard<std::mutex> lk(m);
    paced = on;
}

// Called as the ESP32 resets or sleeps: the module's clock, for the next boot.
void sim::gpsPersist() {
    int64_t utc, real;
    {
        // try_lock: this also runs from the SIGABRT handler, which may have
        // interrupted the module's own thread holding the lock.
        std::unique_lock<std::mutex> lk(m, std::try_to_lock);
        utc = utcBaseUs + (int64_t)sim_clock::micros64();
        real = std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    }
    if (FILE* f = fopen(clockPath().c_str(), "w")) {
        fprintf(f, "%lld %lld\n", (long long)utc, (long long)real);
        fclose(f);
    }
}
