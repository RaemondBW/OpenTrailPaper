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
bool fix = false;
int sats = 0;
double lat = 0, lon = 0, alt = 50, speedKmh = 0, course = 0;
std::vector<std::pair<double, double>> track;
size_t trackIdx = 0;
bool trackLoop = false;
std::string cmdLine;
int64_t utcBaseUs = 0;   // GPS time at sim boot (real UTC)
uint32_t sentences = 0;

std::string baudPath() { return sim::opts.dir + "/gps_baud.txt"; }

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
    {
        std::lock_guard<std::mutex> lk(m);
        if (fix) advance();
        if (!output) return;
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
            gga << "GNGGA," << hms << ",,,,,0," << sats << ",25.5,,,,,,";
            rmc << "GNRMC," << hms << ",V,,,,,,," << date << ",,,N,V";
            gsa << "GNGSA,A,1,,,,,,,,,,,,,25.5,25.5,25.5,1";
        }
        out = sentence(gga.str()) + sentence(rmc.str()) + sentence(gsa.str());
        if (++sentences % 5 == 0) {
            std::ostringstream gsv;
            gsv << "GPGSV,1,1," << sats << ",01,45,090,38,02,30,180,35,03,60,270,40,04,20,045,30,0";
            out += sentence(gsv.str());
        }
    }
    deliver(out);
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
        utcBaseUs = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
    }
    // What the firmware writes to the module.
    Serial2.sink = [](const uint8_t* b, size_t n) {
        std::lock_guard<std::mutex> lk(m);
        if (Serial2.baudRate() != moduleBaud) return;   // garbage to the module
        for (size_t i = 0; i < n; ++i) {
            char c = (char)b[i];
            if (c == '$') cmdLine.clear();
            if (c == '\n') { command(cmdLine); cmdLine.clear(); continue; }
            if (c != '\r') cmdLine += c;
        }
    };
    std::thread([] {
        pthread_setname_np("gps_module");
        for (;;) {
            sim_clock::sleepMs(1000);
            tick();
        }
    }).detach();
}

void sim::gpsSet(double la, double lo, double al, double sp, double co, bool f, int s) {
    std::lock_guard<std::mutex> lk(m);
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

std::string sim::gpsStatusJson() {
    std::lock_guard<std::mutex> lk(m);
    std::ostringstream o;
    o.precision(10);
    o << "{\"module_baud\":" << moduleBaud << ",\"uart_baud\":" << Serial2.baudRate() << ",\"output\":"
      << (output ? "true" : "false") << ",\"fix\":" << (fix ? "true" : "false") << ",\"lat\":" << lat
      << ",\"lon\":" << lon << ",\"speed_kmh\":" << speedKmh << ",\"track_left\":"
      << (track.empty() ? 0 : (int)(track.size() - trackIdx - 1)) << "}";
    return o.str();
}
