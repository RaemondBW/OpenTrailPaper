// Open e-paper bike computer — LilyGO T5S3 4.7" e-paper PRO
//
// Tasks:
//   gps   — NMEA parsing → shared state
//   ble   — HR / power / cadence sensors → shared state
//   rec   — 1 Hz FIT records to SD while riding
//   ui    — e-paper dashboard + touch

#include <Arduino.h>
#include <Wire.h>
#include <ExtensionIOXL9555.hpp>
#include <bq27220.h>

#include "config.h"
#include "ride_state.h"
#include "gps_service.h"
#include "rtc_clock.h"
#include "settings.h"
#include "routes.h"
#include "ble_sensors.h"
#include "ble_server.h"
#include "ride_recorder.h"
#include "ui_dashboard.h"
#include "board_power.h"
#include "i2c_bus.h"
#include "sd_bus.h"
#include "usb_storage.h"
#include "power_mgmt.h"
#include "aux_sensors.h"
#include "diag.h"
#include <esp_sleep.h>
#include <soc/rtc_cntl_reg.h>
#ifdef DEBUG_EARLY_USB
#include "USB.h"
#endif

#if __has_include("esp_core_dump.h")
#include "esp_core_dump.h"
#define HAVE_COREDUMP 1
#endif

#ifdef EPDC_BOOT_WAIT
// ---------------------------------------------------------------------------
// BRING-UP ONLY — route the ESP-IDF console to the USB-CDC.
//
// Everything the display library says about itself was being thrown away, which
// is the main reason the port has been so hard to debug. Two separate sinks,
// both dead:
//
//   log_w()/log_e() (Arduino macros, and EPD_Painter's PSRAM-fallback warning)
//       -> log_printf() -> ets_printf() -> putc1, and NOTHING installs a putc1
//          handler unless setDebugOutput(true) is called. Dropped on the floor.
//
//   printf() (every "[PWRCTL] ..." line, including the "FATAL: powerctl init
//   failed!" printed immediately before the library's own while(1) hang)
//       -> newlib stdout -> UART0 -> GPIO43. That is BOARD_GPS_TXD: the console
//          UART shares pins with the GPS. Before gps_service::begin() those
//          bytes are transmitted into the GPS module's RX; after it, Serial1
//          re-muxes GPIO43 to UART1 and the console is left driving no pin at
//          all. Either way we never see a character of it.
//
// setDebugOutput(true) fixes the first. The second needs stdout itself pointed
// somewhere else, so register a tiny write-only VFS backed by Serial and reopen
// stdout onto it. Gated with the rest of the bring-up aids — it must not ship.
// ---------------------------------------------------------------------------
#include <esp_heap_caps.h>
#include <esp_vfs.h>

static ssize_t cdcConsoleWrite(int, const void* data, size_t size) {
    return Serial.write((const uint8_t*)data, size);
}

static void routeConsoleToUsb() {
    Serial.setDebugOutput(true);          // ets_printf/log_w/log_e -> USB-CDC

    esp_vfs_t vfs = {};
    vfs.flags = ESP_VFS_FLAG_DEFAULT;
    vfs.write = &cdcConsoleWrite;
    if (esp_vfs_register("/cdc", &vfs, nullptr) != ESP_OK) return;

    FILE* f = fopen("/cdc/0", "w");
    if (!f) return;
    setvbuf(f, nullptr, _IONBF, 0);       // unbuffered: a hang must not eat the
    stdout = f;                           // last line before it
    stderr = f;
}
#endif

SharedRideState g_state;

// Serializes all I2C access (fuel gauge / touch / IO expander / RTC).
SemaphoreHandle_t g_i2cMutex = nullptr;

// Serializes all SD (SPI) access (recorder / map tiles / BLE saves / diag).
SemaphoreHandle_t g_sdMutex = nullptr;

static bool ioExpanderOk = false;

static ExtensionIOXL9555 ioExpander;
static BQ27220 fuelGauge;
static bool fuelGaugeOk = false;

// Everything the BQ27220 will tell us in one pass. Logged RAW (no "looks
// implausible, drop it" filtering) because the whole point is to tell a real
// reading apart from a bus collision — which returns 0 or 0xFFFF and is
// otherwise indistinguishable from a flat battery / missing gauge.
struct GaugeSnapshot {
    uint16_t soc = 0, mv = 0, rc = 0, fc = 0, dc = 0, soh = 0, tempRaw = 0;
    int16_t  ma = 0;
    uint16_t battStatus = 0, opStatus = 0;
    bool     charging = false;
};

// Takes the I2C lock ONCE for the whole snapshot (~10 register reads, a couple
// of ms) so the fields describe the same instant. Only called when a line is
// about to be logged, never on the 30 s display poll — see batteryTask.
static void gaugeRead(GaugeSnapshot& g) {
    BQ27220BatteryStatus bs{};
    BQ27220OperationStatus os{};
    i2cLock();
    g.soc     = fuelGauge.getStateOfCharge();
    g.mv      = fuelGauge.getVoltage();
    g.ma      = fuelGauge.getCurrent();
    g.rc      = fuelGauge.getRemainingCapacity();
    g.fc      = fuelGauge.getFullChargeCapacity();
    g.dc      = fuelGauge.getDesignCapacity();
    g.soh     = fuelGauge.getStateOfHealth();
    g.tempRaw = fuelGauge.getTemperature();
    fuelGauge.getBatteryStatus(&bs);
    fuelGauge.getOperationStatus(&os);
    i2cUnlock();
    g.battStatus = bs.full;
    g.opStatus   = os.full;
    g.charging   = !bs.reg.DSG;
}

// The diagnostic tail shared by every gauge log line: temperature, health,
// design capacity and the two status words (raw hex + the bits that explain a
// missing reading). NOTE: no '%' character in here — the phone's diagnostics
// chart takes the FIRST '%' on a "battery:" line as the state-of-charge
// (DiagnosticsView.swift), so only the leading SOC may carry one.
static void gaugeDetail(const GaugeSnapshot& g, char* out, size_t n) {
    BQ27220BatteryStatus bs{};   bs.full = g.battStatus;
    BQ27220OperationStatus os{}; os.full = g.opStatus;
    int t10 = (int)g.tempRaw - 2732;          // gauge reports 0.1 K
    int at  = t10 < 0 ? -t10 : t10;
    snprintf(out, n,
             "%s%d.%dC soh %u dc %umAh batt 0x%04x%s%s%s%s op 0x%04x%s sec%u",
             t10 < 0 ? "-" : "", at / 10, at % 10, g.soh, g.dc,
             g.battStatus,
             bs.reg.BATTPRES ? " pres" : " NOBATT",
             bs.reg.AUTH_GD  ? " auth" : "",
             bs.reg.FC       ? " full" : "",
             bs.reg.SYSDWN   ? " SYSDWN" : "",
             g.opStatus,
             os.reg.INITCOMP ? " init" : " NOINIT",
             os.reg.SEC);
}

// One "battery:" line. The valid-reading form MUST keep its leading
// "<soc>% <mv>mV <ma>mA <rc>/<fc>mAh <charging|discharging>" shape — the phone
// app parses exactly that prefix to draw the drain chart. Detail is appended
// after it. When the SOC is not plausible we log a differently-shaped line with
// no '%' at all, so the phone skips it instead of charting a bogus 0 %.
static void gaugeLogLine(const GaugeSnapshot& g, bool socValid, int tries) {
    char detail[140];
    gaugeDetail(g, detail, sizeof(detail));
    if (socValid) {
        diag::log("battery: %u%% %umV %dmA %u/%umAh %s %s", g.soc, g.mv, g.ma,
                  g.rc, g.fc, g.charging ? "charging" : "discharging", detail);
    } else {
        // tries == 0 means the poll loop never ran (init() failed at boot), so
        // the raw SOC below came from this snapshot rather than a retry round.
        char how[28];
        if (tries > 0) snprintf(how, sizeof(how), "after %d tries", tries);
        else           snprintf(how, sizeof(how), "gauge uninitialised");
        diag::log("battery: NO-SOC raw 0x%04x (%s), %umV %dmA "
                  "rc %u fc %u %s", g.soc, how, g.mv, g.ma, g.rc, g.fc, detail);
    }
}

static void batteryTask(void*) {
    uint32_t lastLog = 0;
    bool firstLog = true;
    uint32_t failStreak = 0;
    // A gauge whose init() failed is still polled here, purely so the log can
    // say WHICH failure it is: registers that answer with a sane SOC prove the
    // chip is on the bus and only the unseal/provisioning in init() went wrong,
    // while 0xFFFF everywhere means nothing is answering at all. The display
    // still shows nothing in this state — the reading is not trusted, because a
    // failed init can also mean a wrong device ID or the wrong battery profile.
    if (!fuelGaugeOk)
        diag::log("battery: gauge init FAILED at boot — logging raw reads only, "
                  "display stays blank");
    for (;;) {
        uint16_t soc = 0;
        int tries = 0;
        if (fuelGaugeOk) {
            // The fuel gauge shares the I2C bus with touch / IO expander / RTC;
            // a colliding read returns 0 or 0xFFFF. Only accept a plausible SOC
            // (1..100) and retry a few times — NEVER overwrite the last good
            // value with a failed read, or the battery display flickers/vanishes.
            for (int i = 0; i < 4; ++i) {
                ++tries;
                i2cLock(); uint16_t v = fuelGauge.getStateOfCharge(); i2cUnlock();
                if (v >= 1 && v <= 100) { soc = v; break; }
                soc = v;                         // keep the raw value for the log
                vTaskDelay(pdMS_TO_TICKS(15));   // unlocked between tries
            }
            bool chg = false;
            i2cLock(); chg = fuelGauge.getIsCharging(); i2cUnlock();
            if (soc >= 1 && soc <= 100) {
                g_state.with([&](RideState& s) {
                    s.batteryPercent = (uint8_t)soc;
                    s.charging = chg;
                });
            }
        }
        bool socValid = fuelGaugeOk && soc >= 1 && soc <= 100;

        // A run of failed reads is exactly the "battery % vanished" symptom, so
        // it gets logged the moment it starts (and again when it ends) instead
        // of leaving a silent gap in the log where the samples should be.
        bool firstFailure = false;
        if (!socValid) {
            firstFailure = (failStreak == 0);
            ++failStreak;
        } else if (failStreak) {
            diag::log("battery: reads recovered after %u failed poll(s) (~%us)",
                      (unsigned)failStreak, (unsigned)(failStreak * 30));
            failStreak = 0;
        }

        // Sample every 2 min so drain rate can be tracked from the diagnostics
        // log. Current is signed: negative = discharging (mA).
        if (firstLog || firstFailure || millis() - lastLog > 120000) {
            firstLog = false;
            lastLog = millis();
            GaugeSnapshot g;
            gaugeRead(g);
            // Report the SOC the display poll actually saw, not a fresh read —
            // a second read that happens to succeed would hide the failure.
            if (fuelGaugeOk) g.soc = soc;
            gaugeLogLine(g, socValid, tries);
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

static const char* resetReasonStr(esp_reset_reason_t r) {
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

// Which wake source actually ended the last deep sleep. Only EXT0 (the BOOT
// button) is ever armed by shutdownDevice(), so anything else in this log means
// a stale trigger survived into deep sleep — the failure mode where the device
// "turns itself back on" a few seconds after a power-off. Worth a line: without
// it, every wake looks identical in the log ("reset: wake from sleep").
static const char* wakeCauseStr(esp_sleep_wakeup_cause_t c) {
    switch (c) {
        case ESP_SLEEP_WAKEUP_EXT0:    return "BOOT button (ext0)";
        case ESP_SLEEP_WAKEUP_EXT1:    return "ext1 UNEXPECTED";
        case ESP_SLEEP_WAKEUP_TIMER:   return "RTC timer UNEXPECTED";
        case ESP_SLEEP_WAKEUP_GPIO:    return "gpio UNEXPECTED";
        case ESP_SLEEP_WAKEUP_UART:    return "uart UNEXPECTED";
        case ESP_SLEEP_WAKEUP_TOUCHPAD:return "touchpad UNEXPECTED";
        case ESP_SLEEP_WAKEUP_ULP:     return "ulp UNEXPECTED";
        case ESP_SLEEP_WAKEUP_UNDEFINED: return "none (not a sleep wake)";
        default:                       return "other UNEXPECTED";
    }
}

// After a panic, the ESP32 auto-writes a full core dump to the `coredump`
// flash partition (enabled in the Arduino sdkconfig). At the next boot we
// summarize it — crashing task + program counter + backtrace — into the SD
// diag log so a crash is diagnosable without a serial monitor, then erase it.
// Decode the backtrace PCs offline with:
//   xtensa-esp32s3-elf-addr2line -e .pio/build/t5s3-pro/firmware.elf <PC …>
static void logCoreDumpIfAny() {
#ifdef HAVE_COREDUMP
    esp_core_dump_summary_t* s =
        (esp_core_dump_summary_t*)malloc(sizeof(esp_core_dump_summary_t));
    if (!s) return;
    if (esp_core_dump_get_summary(s) == ESP_OK) {
        diag::log("CRASH dump: task '%s' PC=0x%08x", s->exc_task,
                  (unsigned)s->exc_pc);
        char bt[220];
        int o = 0;
        for (uint32_t i = 0; i < s->exc_bt_info.depth &&
                             o < (int)sizeof(bt) - 12; ++i) {
            o += snprintf(bt + o, sizeof(bt) - o, "0x%08x ",
                          (unsigned)s->exc_bt_info.bt[i]);
        }
        diag::log("CRASH backtrace%s: %s",
                  s->exc_bt_info.corrupted ? " (corrupt)" : "", bt);
        esp_core_dump_image_erase();   // consumed — don't re-log next boot
    }
    free(s);
#endif
}

void setup() {
    // NOTE: the CPU runs at the default 240 MHz. An experimental 160 MHz
    // downclock (for power saving) was REMOVED — the OPI PSRAM couldn't handle
    // the lower clock and the SoC panicked, and the RTC-flag "self-recovery"
    // didn't survive the panic, so the device boot-looped (crash right after
    // "map: embedded default"). Do not re-add setCpuFrequencyMhz() below default
    // without confirming octal-PSRAM stability first.

    Serial.begin(115200);
#ifdef DEBUG_EARLY_USB
    // Diagnostic only. Normally TinyUSB is not started until usb_storage::begin()
    // calls USB.begin() (usb_storage.cpp), which happens LATE — from the UI task,
    // after the SD mount — and is skipped entirely when the card reports zero
    // sectors. So anything that hangs or panics at or before the SD step produces
    // no USB device at all and therefore no log, which is exactly the case one
    // needs the log for. Starting the stack here makes the port enumerate before
    // the SD is touched. Cost: MSC is registered after USB.begin(), so the card
    // does NOT appear as a USB drive in this build. Debug builds only.
    USB.begin();
#endif
    delay(200);
#ifdef EPDC_BOOT_WAIT
    // Bring-up aid: hold here until a serial host attaches (DTR asserted), up to
    // 8 s. USB-CDC discards everything written before the host opens the port, and
    // this board only re-enumerates its OTG port on a physical RST — so catching
    // the early boot lines otherwise means winning a race against a human pressing
    // a button. If setup() hangs, those lines are the only evidence there is.
    // Build-flag gated: it must NOT ship, since with no host attached it adds 8 s
    // to every cold boot.
    while (!Serial && millis() < 8000) delay(50);
    delay(300);
    routeConsoleToUsb();   // must precede anything that logs, incl. the panel
#endif
    Serial.println("\n[main] e-paper bike computer booting");

    diag::begin();
    esp_reset_reason_t rr = esp_reset_reason();
    diag::log("boot firmware %s (reset: %s [%d])", FIRMWARE_VERSION,
              resetReasonStr(rr), (int)rr);
    // Is the force-download-boot bit still set?
    //
    // The `bootloader` console command (usb_persist_restart) sets it and NOTHING
    // in the Arduino core ever clears it — whether it survives depends on the
    // ROM, which we can't read from the host. If this logs 1 on a normal app
    // boot, the bit is sticky and the command strands the device in download
    // mode until a real power cycle; if it logs 0, the command is safe to use
    // for hands-free flashing and the physical BOOT/RST dance is unnecessary.
    diag::log("force-download-boot bit: %d",
              (int)((REG_READ(RTC_CNTL_OPTION1_REG) & RTC_CNTL_FORCE_DOWNLOAD_BOOT) != 0));
    if (rr == ESP_RST_DEEPSLEEP) {
        esp_sleep_wakeup_cause_t wc = esp_sleep_get_wakeup_cause();
        diag::log("woke by: %s [%d]", wakeCauseStr(wc), (int)wc);
    }
    if (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT)
        logCoreDumpIfAny();          // save the backtrace to SD after a crash
    diag::log("cpu %d MHz", getCpuFrequencyMhz());
    g_state.begin();
    g_i2cMutex = xSemaphoreCreateMutex();   // guard the shared I2C bus
    g_sdMutex = xSemaphoreCreateRecursiveMutex();  // guard the shared SD bus
    Wire.begin(BOARD_SDA, BOARD_SCL);

    // GPS (and LoRa) 3V3 rail is gated by the IO expander.
    if (ioExpander.init(Wire, BOARD_SDA, BOARD_SCL, XL9555_SLAVE_ADDRESS0)) {
        ioExpanderOk = true;
        ioExpander.pinMode(IOEXP_PIN_RADIO_POWER, OUTPUT);
        ioExpander.digitalWrite(IOEXP_PIN_RADIO_POWER, HIGH);
        // Side button input (PC12 = expander pin 10)
        ioExpander.pinMode(IOEXP_PIN_SIDE_BUTTON, INPUT);
        delay(100);
    } else {
        Serial.println("[main] IO expander not found — GPS may be unpowered");
    }
    pinMode(BOARD_BOOT_BTN, INPUT_PULLUP);

    fuelGaugeOk = fuelGauge.init();
    // init() failing kills the battery display for the whole session, so record
    // it — with the device number, which separates "wrong/absent chip" (anything
    // but 0x0220) from "right chip, but unseal or profile check refused".
    diag::log("gauge: init %s, device 0x%04x (expect 0x0220)",
              fuelGaugeOk ? "ok" : "FAILED", fuelGauge.getDeviceNumber());
    // Prime the battery reading synchronously so the first UI frame after boot
    // (and right after an install) never shows a bogus 0%. The BQ27220 can need
    // a moment to report a valid state-of-charge, so retry briefly for non-zero.
    if (fuelGaugeOk) {
        uint16_t soc = 0, raw = 0;
        int tries = 0;
        for (int i = 0; i < 25; ++i) {                 // retry until a valid read
            ++tries;
            raw = fuelGauge.getStateOfCharge();
            if (raw >= 1 && raw <= 100) { soc = raw; break; }  // ignore 0 / 0xFFFF
            delay(20);
        }
        if (soc >= 1) {
            bool chg = fuelGauge.getIsCharging();
            g_state.with([&](RideState& s) {
                s.batteryPercent = (uint8_t)soc;
                s.charging = chg;
            });
            diag::log("gauge: boot SOC %u%% after %d tr%s", soc, tries,
                      tries == 1 ? "y" : "ies");
        } else {
            // The first frame will show "--%". Say so, and say what the gauge
            // did answer: a steady 0x0000 or 0xFFFF here is the fingerprint of
            // a dead/unresponsive gauge rather than a slow-to-settle one.
            diag::log("gauge: NO valid boot SOC, last raw 0x%04x after %d tries",
                      raw, tries);
        }
    }
    settings::begin();
    g_state.with([](RideState& s) {
        s.ftpW = (uint16_t)settings::ftpWatts();
        s.tzMin = (int16_t)settings::tzMinutes();
        s.useMiles = settings::useMiles();
        s.clock24h = settings::clock24h();
    });
    // Panel FIRST, before anything slow. E-paper holds its last image with no
    // power, so until this runs the glass still shows the previous session and
    // the device looks dead — worst exactly when something is wrong, since a
    // failing SD mount alone burns ~1.5 s in sdWait() timeouts before setup()
    // gets anywhere. beginPanel() touches only the display, touch and input
    // interrupts; the map load stays in begin() below because it needs the SD.
    bool uiOk = ui_dashboard::beginPanel();

    // Announced first: the mount can take a couple of seconds on a cold card,
    // and until this repaint landed the glass sat on "Display" with nothing to
    // say the device was busy — which is what read as a freeze.
    ui_dashboard::bootStep("SD card");
    bool sdOk = ride_recorder::begin();
    if (sdOk)
        ui_dashboard::bootDetailFor("SD card", "%lu MB free · %d rides",
                                    (unsigned long)ride_recorder::sdFreeMB(),
                                    ride_recorder::rideCount());
    else
        ui_dashboard::bootDetailFor("SD card", "no card — logging off");
    ui_dashboard::bootStatus("SD card", sdOk);
    if (sdOk) {
        routes::begin();
    }
    // Restore the wall clock from the coin-cell RTC (which, unlike the ESP32's
    // internal clock, survives a full power-off). Must happen before the GPS
    // warm-start seed below so time-aiding fires on a cold boot too — a cold
    // start with a known position AND time is far faster than position alone.
    // Only trust the RTC once GPS has written UTC to it at least once: from the
    // factory it can hold LOCAL time (observed 8 h off UTC), and seeding a
    // grossly wrong time into the receiver hurts acquisition rather than helps.
    ui_dashboard::bootStep("RTC");
    if (rtc_clock::begin()) {

        time_t rt;
        if (!settings::rtcTrusted()) {
            diag::log("rtc: present, not yet GPS-validated (ignoring for aiding)");
            ui_dashboard::bootDetailFor("RTC", "present · not GPS-set");
            ui_dashboard::bootStatus("RTC", true);
        } else if (rtc_clock::read(rt)) {
            struct timeval tv = {rt, 0};
            settimeofday(&tv, nullptr);
            diag::log("rtc: clock restored (%ld)", (long)rt);
            ui_dashboard::bootDetailFor("RTC", "clock restored");
            ui_dashboard::bootStatus("RTC", true);
        } else {
            diag::log("rtc: trusted but read invalid (VL set?)");
        }
    } else {
        ui_dashboard::bootDetailFor("RTC", "not found");
        ui_dashboard::bootStatus("RTC", false);
        diag::log("rtc: not found");
    }
    // NOTE: usb_storage::begin() is called from the UI task AFTER the boot-time
    // SD firmware-update check, so a firmware.bin dropped on the card always
    // flashes before the computer can mount (and grab) the SD.
    ui_dashboard::bootStep("GPS");
    bool gpsOk = gps_service::begin();
    ui_dashboard::bootDetailFor("GPS", "%s · %s", gps_service::moduleName(),
                                gpsOk ? "aiding sent" : "no data");
    ui_dashboard::bootStatus("GPS", gpsOk);
    diag::log("gps module: %s", gps_service::moduleName());
    // Warm-start seed: hand the receiver the last-known position (and time if
    // the system clock survived deep sleep) so it doesn't cold-search the whole
    // sky. Position alone still narrows the search; time is added when valid.
    {
        double alat, alon;
        if (settings::lastPosition(alat, alon)) {
            time_t now = time(nullptr);
            bool haveTime = now > 1735689600;   // clock set since 2025-01-01?
            gps_service::injectAiding(alat, alon, now, haveTime, 50000.0f, 30.0f);
            diag::log("gps warm-start seed: %.4f,%.4f time=%d", alat, alon, haveTime);
        } else {
            diag::log("gps warm-start: no saved position");
        }
    }
    // Display BEFORE the two NimBLE stacks, which is the opposite of the order
    // that shipped on epdiy.
    //
    // Internal DRAM does not fit everything: ~137 KB static + ~160 KB for
    // EPD_Painter's default layout + ~50 KB for the BLE controller, against
    // 320 KB. Measured both ways, and each order breaks the other subsystem:
    //
    //   BLE first     - 147 KB contiguous left, so the 129,600-byte fastbuffer
    //                   still fits internal and eats it; dec_sweeps then wants
    //                   12,960 with 11,764 left and EPD_Painter::begin() returns
    //                   false SILENTLY at EPD_Painter.cpp:780. Blank screen.
    //   Display first - display fine, but NimBLEDevice::init() then hangs with
    //                   46 KB free / 34 KB largest. setup() never finishes.
    //
    // What actually makes it fit is epdc_begin() steering the fastbuffer into
    // PSRAM (see the ballast comment there), which frees ~129 KB. With that in
    // place the order is no longer load-bearing — this one is kept only because
    // it puts the panel up early, and boot now ends with ~124 KB internal spare.
    // The [epdc]/[main] heap lines exist to make any regression obvious.
    ui_dashboard::bootDetailFor("Display", "epd_painter 540x960");
    ui_dashboard::bootStatus("Display", uiOk);   // panel came up back at the top
    ui_dashboard::begin();
#ifdef EPDC_BOOT_WAIT
    // Bring-up trace. setup() now reaches "begin() done" and then stops before
    // "[main] all tasks started" — no panic, no reset, the USB CDC stays up, so
    // it is a hang, not a crash. The suspect is RAM: epdc_begin() reports only
    // ~49 KB of INTERNAL left, and NimBLEDevice::init() brings up the controller,
    // which wants internal/DMA-capable memory. Log the internal heap either side
    // of each remaining init so the culprit and its headroom are unambiguous.
#define BOOT_STEP(msg)                                                        \
    Serial.printf("[main] %lu ms: %s (internal=%u largest=%u)\n",             \
                  (unsigned long)millis(), msg,                               \
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),     \
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL))
#else
#define BOOT_STEP(msg) ((void)0)
#endif
    // Take the side button back off the display driver.
    //
    // epd_painter_powerctl::begin() runs `for (pin = 8; pin <= 13) pcaPinMode(pin,
    // PCA_OUTPUT)`, but its own map only uses 8/9/11/12/13 (OE, MODE, PWRUP,
    // VCOM, WAKEUP). Pin 10 is not the driver's at all — it is our side button —
    // and the blanket loop turns it into an output, so board_side_button_pressed()
    // reads a driven pin and the button stops working. The expander is shared;
    // the driver just assumes the whole upper port is its own.
    //
    // Re-assert it after the panel is up. XL9555::pinMode() read-modify-writes
    // the config register, so this touches bit 10 only and leaves the driver's
    // rail pins alone.
    if (ioExpanderOk) ioExpander.pinMode(IOEXP_PIN_SIDE_BUTTON, INPUT);

    BOOT_STEP("ui_dashboard done -> ble_sensors::begin()");
    ble_sensors::begin();
    BOOT_STEP("ble_sensors done -> ble_server::begin()");
    ble_server::begin();   // GATT server for the iOS companion app
    // Optional Qwiic sensors. Probed AFTER the panel and the other I2C devices
    // so a chip that is not there cannot delay anything that is.
    //
    // Reported on the boot screen like every other subsystem. "Optional" is not
    // a reason to stay silent — the opposite: a plugged-in board that did not
    // answer is exactly the thing a rider needs told, and with no line at all
    // the only way to find out was the serial console.
    ui_dashboard::bootStep("Qwiic");
    const bool auxOk = aux_sensors::begin();
    if (auxOk) {
        ui_dashboard::bootDetailFor("Qwiic", "%s%s%s",
                                    aux_sensors::haveBaro() ? "baro " : "",
                                    aux_sensors::haveCompass() ? "compass " : "",
                                    aux_sensors::haveMotion() ? "motion" : "");
    } else {
        ui_dashboard::bootDetailFor("Qwiic", "none attached");
    }
    // Not a failure when nothing is plugged in: the device is complete without
    // them, and a red line for a board the rider never fitted is a bug report
    // waiting to happen.
    ui_dashboard::bootStatus("Qwiic", true);

    BOOT_STEP("ble_server done -> power_mgmt::begin()");

    // Enable automatic light sleep now that every peripheral (GPS UART, BLE, EPD)
    // is up. No-op + logged warning on a stock framework without CONFIG_PM_ENABLE.
    power_mgmt::begin();
    BOOT_STEP("power_mgmt done -> creating tasks");

    xTaskCreatePinnedToCore(gps_service::task, "gps", 4096, nullptr, 3, nullptr, 0);
    xTaskCreatePinnedToCore(ble_sensors::task, "ble", 6144, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(ble_server::task, "srv", 4096, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(ride_recorder::task, "rec", 6144, nullptr, 3, nullptr, 1);
    xTaskCreatePinnedToCore(batteryTask, "bat", 3072, nullptr, 1, nullptr, 1);
    // No task at all when nothing is fitted — the whole feature costs an absent
    // device exactly one bus probe at boot.
    if (auxOk)
        xTaskCreatePinnedToCore(aux_sensors::task, "aux", 3072, nullptr, 1, nullptr, 1);
    xTaskCreatePinnedToCore(ui_dashboard::task, "ui", 8192, nullptr, 2, nullptr, 1);

    Serial.println("[main] all tasks started");
}

void loop() {
    usb_storage::poll();   // reclaim the SD when the host disconnects
    ride_recorder::retryMountIfNeeded();   // pick a dropped card back up
    power_mgmt::tick();    // hold light sleep off while the USB console is open
    vTaskDelay(pdMS_TO_TICKS(1000));
}

void board_radio_power(bool on) {
    if (!ioExpanderOk) return;
    i2cLock();
    ioExpander.digitalWrite(IOEXP_PIN_RADIO_POWER, on ? HIGH : LOW);
    i2cUnlock();
}

bool board_read_power(uint16_t& mv, int16_t& ma) {
    if (!fuelGaugeOk) return false;
    i2cLock();
    mv = fuelGauge.getVoltage();
    ma = fuelGauge.getCurrent();
    i2cUnlock();
    return true;
}

bool board_gauge_report(char* out, size_t n) {
    GaugeSnapshot g;
    gaugeRead(g);                      // read even when init() failed — the raw
    char detail[140];                  // registers are the evidence we want
    gaugeDetail(g, detail, sizeof(detail));
    snprintf(out, n, "soc %u%s %umV %dmA %u/%umAh %s %s", g.soc,
             (g.soc >= 1 && g.soc <= 100) ? "%" : "% (INVALID)", g.mv, g.ma,
             g.rc, g.fc, g.charging ? "charging" : "discharging", detail);
    return fuelGaugeOk;
}

// TWO agreeing reads, not one — this is the only button whose state arrives
// over I2C, and a failed I2C read is indistinguishable from a press.
//
// SensorLib's digitalRead() ends in getRegisterBit(), which returns FALSE when
// the transaction fails (readRegister == DEV_WIRE_ERR). False is LOW, and LOW
// is exactly what this button reads when it is held down — so any glitched read
// looked like a press, and since the UI acts on the RELEASE, the very next good
// read fired the backlight. One corrupted transaction, one phantom press.
//
// Glitched reads are not hypothetical on this bus. i2c_bus.h says it plainly:
// concurrent transactions from different tasks corrupt each other. The panel
// driver power-cycles this same XL9555 (its rails are pins 8-13, ours is 10)
// every time the display sleeps and wakes, and it does that on its own private
// mutex without taking i2cLock() — so its transactions can and do land in the
// middle of somebody else's. The expander's INT line makes it worse: it fires
// on ANY input change, including the two TPS power-good pins the driver watches,
// so the button gets polled every time the panel cycles, not just when a finger
// touches it.
//
// A 3 ms gap between the two reads is enough to be past a colliding transaction
// (a byte at 400 kHz is ~25 us) while still being a single glance at the pin.
bool board_side_button_pressed() {
    if (!ioExpanderOk) return false;
    i2cLock();
    bool first = ioExpander.digitalRead(IOEXP_PIN_SIDE_BUTTON) == LOW;
    bool second = false;
    if (first) {
        delayMicroseconds(3000);
        second = ioExpander.digitalRead(IOEXP_PIN_SIDE_BUTTON) == LOW;
    }
    i2cUnlock();
    if (first && !second) {
        // Rate-limited so a noisy bus can't flood the log.
        static uint32_t lastLog = 0;
        if (millis() - lastLog > 30000) {
            lastLog = millis();
            diag::log("side btn: glitched read ignored (I2C contention)");
        }
    }
    return first && second;
}
