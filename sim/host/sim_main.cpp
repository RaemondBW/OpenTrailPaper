// The firmware sim's entry point: src/main.cpp's setup() and loop(), step for
// step, minus the board (IO expander, fuel gauge, RTC chip). Everything after
// the hardware bring-up is the firmware's own code.
#include <Arduino.h>
#include <Wire.h>

#include "config.h"
#include "ride_state.h"
#include "gps_service.h"
#include "rtc_clock.h"
#include "settings.h"
#include "routes.h"
#include "segments.h"
#include "ble_sensors.h"
#include "ble_server.h"
#include "link.h"
#include "ride_recorder.h"
#include "ui_dashboard.h"
#include "board_power.h"
#include "i2c_bus.h"
#include "sd_bus.h"
#include "sd_diagnostics.h"
#include "usb_storage.h"
#include "power_mgmt.h"
#include "workout_service.h"
#include "aux_sensors.h"
#include "mesh_service.h"
#include "diag.h"
#include "crash_report.h"
#include "memfault_service.h"
#include "sim.h"
#include "sim_clock.h"

#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

SharedRideState g_state;
SemaphoreHandle_t g_i2cMutex = nullptr;
SemaphoreHandle_t g_sdMutex = nullptr;

sim::Options sim::opts;

namespace {

void usage() {
    fprintf(stderr,
            "otp-sim — the OpenTrailPaper firmware on the host (sim/README.md)\n"
            "  --dir DIR              device state: NVS, bonds, OTA image (default sim-device)\n"
            "  --sd DIR               the SD card's root (default DIR/sd)\n"
            "  --ble-port N           simble, the phone's link (default 7878)\n"
            "  --ctl-port N           control channel (default 7879)\n"
            "  --speed F              device clock at F x real time\n"
            "  --gps-baud N           the GPS module's baud at power-on (default: as saved, else 9600)\n"
            "  --gps-cold             the GPS module reports no UTC until it has a fix (indoors)\n"
            "  --wallclock now|UNIX   start with the clock set (the board boots with it unset)\n"
            "  --notify-window N      notifications queued before notify() fails (default 32)\n"
            "  --conn-interval-ms N --packets-per-event K   pace notifications like a slow link\n"
            "  --sd-ready-ms N        the card answers SD.begin() only N ms after power-on\n"
            "  --sd-crash-on-read P   reading card file P resets the device (a file that kills it)\n"
            "  --sd-latency-ms N      every card open/read/write costs N ms of device time (a slow card)\n"
            "  --sd-size-mb N         the card's capacity (default 32 GB); writes fail when it is full\n"
            "  --sd-cluster-kb N      its FAT cluster size (default 32 KB from 2 GB up, else 16 KB)\n"
            "otp-sim card-tool h3|fit ...   build card contents (sim/host/cardtool_sim.cpp)\n"
            "  --lora                 an SX1262 LoRa radio is fitted (the mesh); its air is the control channel\n"
            "  --mac AA:BB:CC:DD:EE:FF  the board's MAC (the mesh node number is bytes 2-5)\n"
            "  --quiet                don't echo the console to stdout\n"
            "  --no-stdin             don't read console commands from stdin\n");
}

// main.cpp's names for the reset reasons, word for word: crash reports carry them.
const char* resetReasonStr(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "power-on / power-loss";
        case ESP_RST_EXT:       return "RST button";
        case ESP_RST_SW:        return "software (OTA / restart)";
        case ESP_RST_PANIC:     return "crash (panic)";
        case ESP_RST_INT_WDT:   return "interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "task watchdog";
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_DEEPSLEEP: return "wake from sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

void mkdirs(const std::string& p) {
    for (size_t i = 1; i < p.size(); ++i)
        if (p[i] == '/') mkdir(p.substr(0, i).c_str(), 0755);
    mkdir(p.c_str(), 0755);
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[main] e-paper bike computer booting");
    diag::begin();
    esp_reset_reason_t rr = esp_reset_reason();
    diag::log("boot firmware %s (sim) (reset: %s [%d])", FIRMWARE_VERSION, resetReasonStr(rr), (int)rr);
    memfault_service::begin((int)rr);
    crash_report::begin((int)rr, resetReasonStr(rr), FIRMWARE_VERSION);
    g_state.begin();
    g_i2cMutex = xSemaphoreCreateMutex();
    g_sdMutex = xSemaphoreCreateRecursiveMutex();
    sim_name_sem(g_i2cMutex, "i2cLock");
    sim_name_sem(g_sdMutex, "sdLock");
    power_mgmt::prepare();
    Wire.begin(BOARD_SDA, BOARD_SCL);
    pinMode(BOARD_BOOT_BTN, INPUT_PULLUP);
    sim::setBattery(100, false);   // the fuel gauge, set by the control channel from here
    settings::begin();
    g_state.with([](RideState& s) {
        s.ftpW = (uint16_t)settings::ftpWatts();
        s.tzMin = (int16_t)settings::tzMinutes();
        s.useMiles = settings::useMiles();
        s.clock24h = settings::clock24h();
    });
    bool uiOk = ui_dashboard::beginPanel();
    ui_dashboard::bootStep("SD card");
    bool sdOk = ride_recorder::begin();
    if (sdOk) crash_report::onSdMounted("boot mount");
    if (sdOk)
        ui_dashboard::bootDetailFor("SD card", "%lu MB free · %d rides",
                                    (unsigned long)ride_recorder::sdFreeMB(), ride_recorder::rideCount());
    else if (ride_recorder::sdCardState() == SD_CARD_UNREADABLE)
        ui_dashboard::bootDetailFor("SD card", "unreadable · FAT32?");
    else
        ui_dashboard::bootDetailFor("SD card", "No SD card");
    ui_dashboard::bootStatus("SD card", sdOk);
    if (sdOk) {
        routes::begin();
        segments::begin();
    }
    ui_dashboard::bootStep("RTC");
    ui_dashboard::bootDetailFor("RTC", "not found");
    ui_dashboard::bootStatus("RTC", false);
    diag::log("rtc: not found");
    ui_dashboard::bootStep("GPS");
    bool gpsOk = gps_service::begin();
    ui_dashboard::bootDetailFor("GPS", "%s · %s", gps_service::moduleName(), gpsOk ? "aiding sent" : "no data");
    ui_dashboard::bootStatus("GPS", gpsOk);
    diag::log("gps module: %s", gps_service::moduleName());
    ui_dashboard::bootStep("Mesh");
    const bool meshOk = mesh_service::begin();
    if (!mesh_service::enabled())
        ui_dashboard::bootDetailFor("Mesh", "off in settings");
    else if (mesh_service::radioOk())
        ui_dashboard::bootDetailFor("Mesh", "%s SF%u %.3f", mesh_service::channelName(),
                                    mesh::preset(mesh_service::presetIndex()).sf, mesh_service::frequencyMHz());
    else
        ui_dashboard::bootDetailFor("Mesh", "no SX1262");
    ui_dashboard::bootStatus("Mesh", mesh_service::radioOk() || !mesh_service::enabled());
    ui_dashboard::bootStep("Qwiic");
    ui_dashboard::bootDetailFor("Qwiic", "none attached");
    ui_dashboard::bootStatus("Qwiic", true);
    {
        double alat, alon;
        if (settings::lastPosition(alat, alon)) {
            time_t now = time(nullptr);
            bool haveTime = now > 1735689600;
            gps_service::injectAiding(alat, alon, now, haveTime, 50000.0f, 30.0f);
            diag::log("gps warm-start seed: %.4f,%.4f time=%d", alat, alon, haveTime);
        } else {
            diag::log("gps warm-start: no saved position");
        }
    }
    ui_dashboard::bootDetailFor("Display", "sim 540x960");
    ui_dashboard::bootStatus("Display", uiOk);
    ui_dashboard::begin();
    ble_sensors::begin();
    ble_server::begin();
    power_mgmt::begin();
    workout_service::begin();
    xTaskCreatePinnedToCore(gps_service::task, "gps", 4096, nullptr, 3, nullptr, 0);
    xTaskCreatePinnedToCore(ble_sensors::task, "ble", 6144, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(ble_server::task, "srv", 4096, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(phone_link::task, "link", phone_link::TASK_STACK, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(ride_recorder::task, "rec", 6144, nullptr, 3, nullptr, 1);
    if (meshOk) xTaskCreatePinnedToCore(mesh_service::task, "mesh", 6144, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(ui_dashboard::task, "ui", 12288, nullptr, 2, nullptr, 1);
    diag::log("boot: all tasks started");
    diag::finishBoot();
}

void loop() {
    usb_storage::poll();
    ride_recorder::retryMountIfNeeded();
    power_mgmt::tick();
    sd_diagnostics::tick();
    crash_report::tick();
    memfault_service::tick();
    workout_service::tick();
    workout_service::serviceFor(1000);
}

}  // namespace

// ---- resets: the reset reason and RTC memory across a re-exec ------------------------
//
// <dir>/rtc_mem.bin holds what the next boot needs from the chip: the reset
// reason and the RTC_NOINIT_ATTR section (esp_sim.h). Written just before a
// reset or deep sleep, consumed by the next boot; absent = a power-on.
namespace {
int g_resetReason = ESP_RST_POWERON;
constexpr char kRtcMagic[8] = {'O', 'T', 'P', 'R', 'T', 'C', '1', 0};

uint8_t* rtcSection(size_t* n) {
    unsigned long sz = 0;
    uint8_t* p = getsectiondata(&_mh_execute_header, "__DATA", "__rtc_noinit", &sz);
    *n = p ? (size_t)sz : 0;
    return p;
}
std::string rtcPath() { return sim::opts.dir + "/rtc_mem.bin"; }

// Plain write(2): also called from the SIGABRT handler.
void saveRtc(int reason) {
    std::string path = rtcPath();
    if (reason == ESP_RST_POWERON) { unlink(path.c_str()); return; }
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    size_t n = 0;
    uint8_t* p = rtcSection(&n);
    uint64_t len = n;
    int32_t r = reason;
    (void)!write(fd, kRtcMagic, sizeof kRtcMagic);
    (void)!write(fd, &r, sizeof r);
    (void)!write(fd, &len, sizeof len);
    if (p && n) (void)!write(fd, p, n);
    close(fd);
}

void restoreRtc() {
    std::string path = rtcPath();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;
    char magic[8];
    int32_t r = 0;
    uint64_t len = 0;
    if (fread(magic, 1, 8, f) == 8 && !memcmp(magic, kRtcMagic, 8) && fread(&r, sizeof r, 1, f) == 1 &&
        fread(&len, sizeof len, 1, f) == 1) {
        g_resetReason = r;
        size_t n = 0;
        uint8_t* p = rtcSection(&n);
        // A different build (OTA) may lay the section out differently: keep
        // the reason, start RTC memory clean, as a new image's would read garbage.
        if (p && len == n && fread(p, 1, n, f) != n) memset(p, 0, n);
    }
    fclose(f);
    unlink(path.c_str());
}

// The firmware's abort() — `crash panic` on the console, a failed assert — is a
// panic: the chip resets with ESP_RST_PANIC and RTC memory intact.
void onAbort(int) { sim::reboot("panic: abort()", ESP_RST_PANIC); }
}  // namespace

extern "C" esp_reset_reason_t esp_reset_reason(void) { return (esp_reset_reason_t)g_resetReason; }
int sim::resetReason() { return g_resetReason; }
int sim::resetReasonByName(const std::string& n) {
    if (n == "poweron") return ESP_RST_POWERON;
    if (n == "ext") return ESP_RST_EXT;
    if (n == "sw") return ESP_RST_SW;
    if (n == "panic") return ESP_RST_PANIC;
    if (n == "int_wdt") return ESP_RST_INT_WDT;
    if (n == "task_wdt") return ESP_RST_TASK_WDT;
    if (n == "wdt") return ESP_RST_WDT;
    if (n == "brownout") return ESP_RST_BROWNOUT;
    return -1;
}

[[noreturn]] void sim::reboot(const char* why, int resetReason) {
    consoleLine(std::string("[sim] reboot: ") + why);
    fflush(stdout);
    saveRtc(resetReason);
    gpsPersist();
    char path[4096];
    uint32_t n = sizeof path;
    if (_NSGetExecutablePath(path, &n) != 0) _exit(3);
    std::vector<char*> argv;
    for (auto& a : opts.argv) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    // Close every socket so the new image can listen on the same ports.
    for (int fd = 3; fd < 1024; ++fd) close(fd);
    execv(path, argv.data());
    _exit(3);
}

[[noreturn]] void sim::powerOff(const char* why) {
    consoleLine(std::string("[sim] powered off: ") + why);
    fflush(stdout);
    saveRtc(ESP_RST_DEEPSLEEP);
    gpsPersist();
    _exit(0);
}

int main(int argc, char** argv) {
    bool readStdin = true;
    std::string wallclock, gpsBaud;
    if (argc > 1 && !strcmp(argv[1], "card-tool")) return sim::cardTool(argc - 2, argv + 2);
    for (int i = 0; i < argc; ++i) sim::opts.argv.push_back(argv[i]);
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { usage(); exit(1); }
            return argv[++i];
        };
        if (a == "--dir") sim::opts.dir = next();
        else if (a == "--sd") sim::opts.sdDir = next();
        else if (a == "--ble-port") sim::opts.blePort = atoi(next().c_str());
        else if (a == "--ctl-port") sim::opts.ctlPort = atoi(next().c_str());
        else if (a == "--speed") sim::opts.speed = atof(next().c_str());
        else if (a == "--gps-baud") gpsBaud = next();
        else if (a == "--gps-cold") sim::opts.gpsCold = true;
        else if (a == "--wallclock") wallclock = next();
        else if (a == "--notify-window") sim::opts.notifyWindow = atoi(next().c_str());
        else if (a == "--conn-interval-ms") sim::opts.connIntervalMs = atoi(next().c_str());
        else if (a == "--packets-per-event") sim::opts.packetsPerEvent = atoi(next().c_str());
        else if (a == "--sd-ready-ms") sim::opts.sdReadyMs = atoi(next().c_str());
        else if (a == "--sd-crash-on-read") sim::opts.sdCrashOnRead = next();
        else if (a == "--sd-latency-ms") sim::setSdLatency(atoi(next().c_str()));
        else if (a == "--sd-size-mb") sim::opts.sdSizeMb = strtoull(next().c_str(), nullptr, 10);
        else if (a == "--sd-cluster-kb") sim::opts.sdClusterKb = (uint32_t)atoi(next().c_str());
        else if (a == "--lora") sim::opts.lora = true;
        else if (a == "--mac") {
            unsigned m[6];
            if (sscanf(next().c_str(), "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
                usage();
                return 1;
            }
            for (int k = 0; k < 6; ++k) sim::opts.mac[k] = (uint8_t)m[k];
        }
        else if (a == "--quiet") sim::opts.quiet = true;
        else if (a == "--no-stdin") readStdin = false;
        else { usage(); return a == "--help" || a == "-h" ? 0 : 1; }
    }
    mkdirs(sim::opts.dir);
    restoreRtc();
    signal(SIGABRT, onAbort);
    if (!gpsBaud.empty()) {
        // Only on the first boot of this device directory: a reboot keeps
        // whatever baud the firmware left the module at.
        std::string p = sim::opts.dir + "/gps_baud.txt";
        struct stat st;
        if (stat(p.c_str(), &st) != 0)
            if (FILE* f = fopen(p.c_str(), "w")) { fprintf(f, "%s\n", gpsBaud.c_str()); fclose(f); }
    }
    if (!wallclock.empty()) {
        int64_t unix = wallclock == "now"
            ? std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()
            : atoll(wallclock.c_str());
        sim_clock::setWallMicros(unix * 1000000);
    }
    sim_clock::setFactor(sim::opts.speed);
    setvbuf(stdout, nullptr, _IOLBF, 0);

    sim::controlBegin();
    sim::bleBegin();
    sim::gpsBegin();
    if (readStdin)
        std::thread([] {
            char line[512];
            while (fgets(line, sizeof line, stdin)) {
                std::string l(line);
                while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
                sim::feedConsole(l);
            }
        }).detach();
    sim::consoleLine("[sim] simble on 127.0.0.1:" + std::to_string(sim::opts.blePort) + ", control on 127.0.0.1:" +
                     std::to_string(sim::opts.ctlPort) + ", device dir " + sim::opts.dir);
    setup();
    for (;;) loop();
}
