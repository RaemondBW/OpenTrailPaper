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
#include "diag.h"

#if __has_include("esp_core_dump.h")
#include "esp_core_dump.h"
#define HAVE_COREDUMP 1
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
    });
    if (ride_recorder::begin()) {
        routes::begin();
    }
    // NOTE: usb_storage::begin() is called from the UI task AFTER the boot-time
    // SD firmware-update check, so a firmware.bin dropped on the card always
    // flashes before the computer can mount (and grab) the SD.
    gps_service::begin();
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
    ble_sensors::begin();
    ble_server::begin();   // GATT server for the iOS companion app
    ui_dashboard::begin();

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
    vTaskDelay(pdMS_TO_TICKS(1000));
}

void board_radio_power(bool on) {
    if (!ioExpanderOk) return;
    i2cLock();
    ioExpander.digitalWrite(IOEXP_PIN_RADIO_POWER, on ? HIGH : LOW);
    i2cUnlock();
}

bool board_side_button_pressed() {
    if (!ioExpanderOk) return false;
    i2cLock();
    bool pressed = ioExpander.digitalRead(IOEXP_PIN_SIDE_BUTTON) == LOW;
    i2cUnlock();
    return pressed;
}
