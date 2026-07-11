#include "gps_service.h"

#include <Arduino.h>
#include <TinyGPS++.h>

#include "config.h"
#include "ride_state.h"
#include "routes.h"
#include "settings.h"

#define SerialGPS Serial2

namespace {

TinyGPSPlus gps;
// Satellites in view, per constellation (GSV term 3 repeats the total).
TinyGPSCustom gpgsvInView(gps, "GPGSV", 3);   // GPS
TinyGPSCustom glgsvInView(gps, "GLGSV", 3);   // GLONASS
TinyGPSCustom bdgsvInView(gps, "GBGSV", 3);   // BeiDou (L76K uses GB)
// C/N0 of the up-to-four satellites in each GPGSV message (terms 7/11/
// 15/19) — signal strength is the decisive weak-signal-vs-obstruction
// metric. bestSnr tracks the strongest we currently hear.
TinyGPSCustom gpgsvSnr0(gps, "GPGSV", 7);
TinyGPSCustom gpgsvSnr1(gps, "GPGSV", 11);
TinyGPSCustom gpgsvSnr2(gps, "GPGSV", 15);
TinyGPSCustom gpgsvSnr3(gps, "GPGSV", 19);
int bestSnr = 0;
bool moduleDetected = false;

// Smoothed heading state (EMA over the course unit vector).
float headX = 0, headY = 0;
bool headingPrimed = false;

// Days-from-civil (Howard Hinnant) — TinyGPS gives calendar UTC, FIT wants
// an epoch timestamp and the RTC may not be set yet.
time_t toUnix(int y, unsigned m, unsigned d, unsigned hh, unsigned mm, unsigned ss) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = era * 146097L + static_cast<long>(doe) - 719468L;
    return static_cast<time_t>(days) * 86400 + hh * 3600 + mm * 60 + ss;
}

bool waitForBytes(uint32_t timeoutMs) {
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        if (SerialGPS.available()) {
            while (SerialGPS.available()) SerialGPS.read();
            return true;
        }
        delay(10);
    }
    return false;
}

// L76K modules talk PCAS at 9600; if that fails assume u-blox M10Q at 38400.
bool initL76K() {
    for (int i = 0; i < 3; ++i) {
        SerialGPS.write("$PCAS03,0,0,0,0,0,0,0,0,0,0,,,0,0*02\r\n");
        delay(200);
        while (SerialGPS.available()) SerialGPS.readString();
        SerialGPS.write("$PCAS06,0*1B\r\n");
        uint32_t deadline = millis() + 500;
        while (!SerialGPS.available()) {
            if (millis() > deadline) return false;
            delay(1);
        }
        SerialGPS.setTimeout(50);
        String ver = SerialGPS.readStringUntil('\n');
        if (ver.startsWith("$GPTXT,01,01,02")) {
            // GPS + BDS + GLONASS (all three constellations), all NMEA
            // sentences on, vehicle dynamics
            SerialGPS.write("$PCAS04,7*1E\r\n");
            delay(250);
            SerialGPS.write("$PCAS03,1,1,1,1,1,1,1,1,1,1,,,0,0*02\r\n");
            delay(250);
            SerialGPS.write("$PCAS11,3*1E\r\n");
            return true;
        }
        delay(500);
    }
    return false;
}

}  // namespace

namespace gps_service {

bool begin() {
    SerialGPS.begin(9600, SERIAL_8N1, BOARD_GPS_RXD, BOARD_GPS_TXD);
    delay(100);

    if (initL76K()) {
        Serial.println("[gps] L76K initialized @9600");
        moduleDetected = true;
        return true;
    }

    // Not an L76K — try u-blox M10Q default baud rate. It streams NMEA out
    // of the box, so just verify data is flowing.
    SerialGPS.updateBaudRate(38400);
    if (waitForBytes(2000)) {
        Serial.println("[gps] u-blox M10Q detected @38400");
        moduleDetected = true;
        return true;
    }

    SerialGPS.updateBaudRate(9600);
    if (waitForBytes(2000)) {
        Serial.println("[gps] NMEA stream detected @9600");
        moduleDetected = true;
        return true;
    }

    Serial.println("[gps] no module detected");
    return false;
}

void task(void*) {
    for (;;) {
        while (SerialGPS.available()) {
            gps.encode(SerialGPS.read());
        }

        // Track the strongest C/N0 among the first four GPS satellites of
        // each GSV batch; recompute when a fresh batch arrives.
        if (gpgsvSnr0.isUpdated()) {
            TinyGPSCustom* snrs[4] = {&gpgsvSnr0, &gpgsvSnr1, &gpgsvSnr2,
                                      &gpgsvSnr3};
            int best = 0;
            for (auto* c : snrs) {
                if (c->isValid()) {
                    int v = atoi(c->value());
                    if (v > best) best = v;
                }
            }
            bestSnr = best;
        }

        if (gps.location.isUpdated() && gps.location.isValid()) {
            routes::updateProgress(gps.location.lat(), gps.location.lng());
        }
        if (gps.location.isUpdated() || gps.satellites.isUpdated()) {
            g_state.with([](RideState& s) {
                s.gpsFix = gps.location.isValid() && gps.location.age() < 3000;
                if (s.gpsFix) {
                    s.latitude = gps.location.lat();
                    s.longitude = gps.location.lng();
                    s.everHadFix = true;
                }
                if (gps.altitude.isValid()) s.altitudeM = gps.altitude.meters();
                if (gps.speed.isValid()) s.speedKmh = gps.speed.kmph();

                // Heading: a single fix's course-over-ground is noisy, so
                // smooth it with an exponential moving average over the
                // heading UNIT VECTOR (handles the 0/360 wrap correctly),
                // heavily weighted toward history. Faster travel gives a
                // more trustworthy sample, so the blend weight scales with
                // speed. Only updated while moving; stopped, the map holds
                // the last heading instead of spinning on GPS jitter.
                if (gps.course.isValid() && s.speedKmh > 5.0f) {
                    float rad = gps.course.deg() * (float)M_PI / 180.0f;
                    float nx = cosf(rad), ny = sinf(rad);
                    if (!headingPrimed) {
                        headX = nx;
                        headY = ny;
                        headingPrimed = true;
                    } else {
                        // 0.08 (heavy smoothing) up to 0.20 at speed.
                        float a = 0.08f + 0.006f * (s.speedKmh - 5.0f);
                        if (a > 0.20f) a = 0.20f;
                        headX += a * (nx - headX);
                        headY += a * (ny - headY);
                    }
                    float h = atan2f(headY, headX) * 180.0f / (float)M_PI;
                    if (h < 0) h += 360.0f;
                    s.courseDeg = h;
                }
                if (gps.satellites.isValid()) s.satellites = gps.satellites.value();
                if (gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2025) {
                    s.timeValid = true;
                    s.utc = toUnix(gps.date.year(), gps.date.month(), gps.date.day(),
                                   gps.time.hour(), gps.time.minute(), gps.time.second());
                }
            });
        }

        // Persist the position every 5 min so the map starts at the last
        // known location after a reboot.
        static uint32_t lastPosSave = 0;
        if (gps.location.isValid() && millis() - lastPosSave > 300000) {
            lastPosSave = millis();
            settings::setLastPosition(gps.location.lat(), gps.location.lng());
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void getDebug(GpsDebug& out) {
    out.moduleDetected = moduleDetected;
    out.chars = gps.charsProcessed();
    out.passedCksum = gps.passedChecksum();
    out.failedCksum = gps.failedChecksum();
    out.withFix = gps.sentencesWithFix();
    out.satsInUse = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
    out.satsInView =
        (gpgsvInView.isValid() ? atoi(gpgsvInView.value()) : 0) +
        (glgsvInView.isValid() ? atoi(glgsvInView.value()) : 0) +
        (bdgsvInView.isValid() ? atoi(bdgsvInView.value()) : 0);
    out.bestSnr = bestSnr;
    out.hdop = gps.hdop.isValid() ? gps.hdop.hdop() : 0;
    out.locValid = gps.location.isValid();
    out.locAgeMs = gps.location.isValid() ? gps.location.age() : 0;
    out.lat = gps.location.isValid() ? gps.location.lat() : 0;
    out.lon = gps.location.isValid() ? gps.location.lng() : 0;
    out.altM = gps.altitude.isValid() ? gps.altitude.meters() : 0;
    out.speedKmh = gps.speed.isValid() ? gps.speed.kmph() : 0;
    if (gps.time.isValid()) {
        out.hour = gps.time.hour();
        out.minute = gps.time.minute();
        out.second = gps.time.second();
    }
}

}  // namespace gps_service
