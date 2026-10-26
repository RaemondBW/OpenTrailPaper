// Shared state between the sim's host modules.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include <functional>

namespace sim {

struct Options {
    std::string dir = "sim-device";   // NVS, bonds, OTA image live here
    std::string sdDir;                // the SD card's root (default <dir>/sd)
    int blePort = 7878, ctlPort = 7879;
    bool quiet = false;
    double speed = 1.0;
    int notifyWindow = 32;
    int connIntervalMs = 0, packetsPerEvent = 0;
    bool gpsCold = false;             // the module has no UTC until it has a fix (indoors)
    std::vector<std::string> argv;    // for reboot (re-exec)
};
extern Options opts;

// Console: every line the firmware prints, numbered.
void consoleLine(const std::string& line);
uint64_t logSeq();
std::vector<std::pair<uint64_t, std::string>> logSince(uint64_t seq, size_t max = 5000);
bool waitLog(const std::string& regex, uint32_t timeoutMs, uint64_t since, std::string* hit, uint64_t* hitSeq);
void onLogLine(std::function<void(uint64_t, const std::string&)> cb);
void feedConsole(const std::string& line);   // as if typed on the USB serial

// Inputs
void setButton(const std::string& name, bool down);
bool buttonDown(const std::string& name);
void touch(int x, int y, bool down);
bool touchState(int& x, int& y);
void homeKeyPress();

// The panel
uint8_t* framebuffer();
uint32_t paintCount();
bool writePng(const std::string& path);

// Modules
void gpsBegin();
// `time`: without a fix, whether the module still reports UTC (false: a cold
// receiver indoors, empty time and date fields).
void gpsSet(double lat, double lon, double alt, double speedKmh, double course, bool fix, int sats, bool time = true);
void gpsTrack(const std::vector<std::pair<double, double>>& pts, double speedKmh, bool loop);
void gpsSilent(bool on);   // the module stops talking (loose connector, brownout)
std::string gpsStatusJson();
void bleBegin();
std::string bleStatusJson();
void controlBegin();
void setBattery(int pct, bool charging);
bool sdPresent();
void setSdPresent(bool on);

[[noreturn]] void reboot(const char* why);
[[noreturn]] void powerOff(const char* why);

}  // namespace sim
