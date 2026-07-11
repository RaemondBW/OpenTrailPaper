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

SharedRideState g_state;

static bool ioExpanderOk = false;

static ExtensionIOXL9555 ioExpander;
static BQ27220 fuelGauge;
static bool fuelGaugeOk = false;

static void batteryTask(void*) {
    for (;;) {
        if (fuelGaugeOk) {
            uint16_t soc = fuelGauge.getStateOfCharge();
            bool chg = fuelGauge.getIsCharging();
            g_state.with([&](RideState& s) {
                s.batteryPercent = soc > 100 ? 100 : (uint8_t)soc;
                s.charging = chg;
            });
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[main] e-paper bike computer booting");

    g_state.begin();
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
    settings::begin();
    g_state.with([](RideState& s) {
        s.ftpW = (uint16_t)settings::ftpWatts();
        s.tzMin = (int16_t)settings::tzMinutes();
        s.useMiles = settings::useMiles();
    });
    if (ride_recorder::begin()) {
        routes::begin();
    }
    gps_service::begin();
    // Warm-start seed: hand the receiver the last-known position (and time if
    // the system clock survived deep sleep) so it doesn't cold-search the whole
    // sky. Position alone still narrows the search; time is added when valid.
    {
        double alat, alon;
        if (settings::lastPosition(alat, alon)) {
            time_t now = time(nullptr);
            bool haveTime = now > 1735689600;   // clock set since 2025-01-01?
            gps_service::injectAiding(alat, alon, now, haveTime, 50000.0f, 30.0f);
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
    vTaskDelay(pdMS_TO_TICKS(1000));
}

void board_radio_power(bool on) {
    if (!ioExpanderOk) return;
    ioExpander.digitalWrite(IOEXP_PIN_RADIO_POWER, on ? HIGH : LOW);
}

bool board_side_button_pressed() {
    if (!ioExpanderOk) return false;
    return ioExpander.digitalRead(IOEXP_PIN_SIDE_BUTTON) == LOW;
}
