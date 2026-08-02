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
#include "diag.h"

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

static void batteryTask(void*) {
    uint32_t lastLog = 0;
    bool firstLog = true;
    for (;;) {
        if (fuelGaugeOk) {
            // The fuel gauge shares the I2C bus with touch / IO expander / RTC;
            // a colliding read returns 0 or 0xFFFF. Only accept a plausible SOC
            // (1..100) and retry a few times — NEVER overwrite the last good
            // value with a failed read, or the battery display flickers/vanishes.
            uint16_t soc = 0;
            for (int i = 0; i < 4; ++i) {
                i2cLock(); uint16_t v = fuelGauge.getStateOfCharge(); i2cUnlock();
                if (v >= 1 && v <= 100) { soc = v; break; }
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
            // Log the battery every 2 min so drain rate can be tracked from the
            // diagnostics log. Current is signed: negative = discharging (mA).
            if (soc >= 1 && (firstLog || millis() - lastLog > 120000)) {
                firstLog = false;
                lastLog = millis();
                i2cLock();
                uint16_t mv = fuelGauge.getVoltage();
                int16_t ma = fuelGauge.getCurrent();
                uint16_t rc = fuelGauge.getRemainingCapacity();
                uint16_t fc = fuelGauge.getFullChargeCapacity();
                i2cUnlock();
                diag::log("battery: %u%% %umV %dmA %u/%umAh %s", soc, mv, ma, rc, fc,
                          chg ? "charging" : "discharging");
            }
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
    // Prime the battery reading synchronously so the first UI frame after boot
    // (and right after an install) never shows a bogus 0%. The BQ27220 can need
    // a moment to report a valid state-of-charge, so retry briefly for non-zero.
    if (fuelGaugeOk) {
        uint16_t soc = 0;
        for (int i = 0; i < 25; ++i) {                 // retry until a valid read
            uint16_t v = fuelGauge.getStateOfCharge();
            if (v >= 1 && v <= 100) { soc = v; break; }  // ignore 0 / 0xFFFF
            delay(20);
        }
        if (soc >= 1) {
            bool chg = fuelGauge.getIsCharging();
            g_state.with([&](RideState& s) {
                s.batteryPercent = (uint8_t)soc;
                s.charging = chg;
            });
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

    bool sdOk = ride_recorder::begin();
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
    if (rtc_clock::begin()) {
        time_t rt;
        if (!settings::rtcTrusted()) {
            diag::log("rtc: present, not yet GPS-validated (ignoring for aiding)");
        } else if (rtc_clock::read(rt)) {
            struct timeval tv = {rt, 0};
            settimeofday(&tv, nullptr);
            diag::log("rtc: clock restored (%ld)", (long)rt);
        } else {
            diag::log("rtc: trusted but read invalid (VL set?)");
        }
    } else {
        diag::log("rtc: not found");
    }
    // NOTE: usb_storage::begin() is called from the UI task AFTER the boot-time
    // SD firmware-update check, so a firmware.bin dropped on the card always
    // flashes before the computer can mount (and grab) the SD.
    bool gpsOk = gps_service::begin();
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
    xTaskCreatePinnedToCore(ui_dashboard::task, "ui", 8192, nullptr, 2, nullptr, 1);

    Serial.println("[main] all tasks started");
}

void loop() {
    usb_storage::poll();   // reclaim the SD when the host disconnects
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

bool board_side_button_pressed() {
    if (!ioExpanderOk) return false;
    i2cLock();
    bool pressed = ioExpander.digitalRead(IOEXP_PIN_SIDE_BUTTON) == LOW;
    i2cUnlock();
    return pressed;
}
