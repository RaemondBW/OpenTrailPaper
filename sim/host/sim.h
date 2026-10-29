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
    int sdReadyMs = 0;                // the card ignores SD.begin() for this long after power-on
    std::string sdCrashOnRead;        // reading this card file resets the device
    uint64_t sdSizeMb = 0;            // the card's capacity (0: 32 GB)
    uint32_t sdClusterKb = 0;         // FAT cluster size (0: as SD Formatter picks for the size)
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
uint32_t scrubCount();   // epdc_clear_dirty() calls
uint32_t clearCount();   // whole-panel epdc_clear() calls
bool writePng(const std::string& path);

// Modules
void gpsBegin();
// `time`: without a fix, whether the module still reports UTC (false: a cold
// receiver indoors, empty time and date fields).
void gpsSet(double lat, double lon, double alt, double speedKmh, double course, bool fix, int sats, bool time = true);
void gpsTrack(const std::vector<std::pair<double, double>>& pts, double speedKmh, bool loop);
void gpsSilent(bool on);   // the module stops talking (loose connector, brownout)
std::string gpsStatusJson();
// What the module's clock says now (UTC seconds; it keeps counting from there):
// a midnight rollover, a week-number rollover, a leap second.
void gpsSetUtc(int64_t unix);
// Bytes from the module as they are (bad checksums, torn sentences, noise).
void gpsRaw(const std::string& bytes);
// Each sentence lands after its bytes have crossed the UART, rather than a
// second's batch at once.
void gpsPaced(bool on);
void gpsPersist();   // the module's clock across an ESP32 reset (its backup supply)
void bleBegin();
std::string bleStatusJson();
void controlBegin();
void setBattery(int pct, bool charging);
bool sdPresent();
void setSdPresent(bool on);
void setSdLatency(int ms);   // device-time cost of every card open/read/write
// The card's capacity and what is on it (fs_sim.cpp). setSdSizeMb swaps in a
// card of another size with the same contents, as if the files were copied over.
struct CardUsage { uint64_t cardBytes, totalBytes, usedBytes, clusterBytes; };
CardUsage sdUsage(bool recount = false);
void setSdSizeMb(uint64_t mb, uint32_t clusterKb = 0);
// `otp-sim card-tool ...`: host-side helpers for building card images.
int cardTool(int argc, char** argv);
// A computer has the card mounted over USB (usb_storage::hostActive): the
// firmware keeps off the card until it is released, then rescans it.
void setUsbHost(bool on);
bool usbHost();

// The function containing `addr` (a return address), demangled; "?" if unknown.
std::string symbolName(void* addr);
// Logs `line` the first time `key` is seen (the sim's own checks).
void warnOnce(const std::string& key, const std::string& line);

// ---- resets (power and robustness) ------------------------------------------------
// A reset re-execs the sim. `resetReason` is the esp_reset_reason_t the next
// boot sees (default 1, ESP_RST_POWERON: a power cut, RTC memory lost); any
// other reason carries the RTC_NOINIT_ATTR variables across, as the chip's RTC
// memory survives a panic, a watchdog, a brownout or esp_restart().
[[noreturn]] void reboot(const char* why, int resetReason = 1);
// Deep sleep: the process exits. The next launch of the same device directory
// is the wake (ESP_RST_DEEPSLEEP, RTC memory kept).
[[noreturn]] void powerOff(const char* why);
int resetReason();                       // what this boot's esp_reset_reason() says
int resetReasonByName(const std::string& name);   // "panic", "task_wdt", ... (-1: unknown)

}  // namespace sim
