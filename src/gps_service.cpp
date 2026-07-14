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
enum GpsKind { GPS_NONE, GPS_CASIC, GPS_UBLOX };
GpsKind moduleKind = GPS_NONE;

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
        Serial.println("[gps] CASIC/L76K initialized @9600");
        moduleDetected = true;
        moduleKind = GPS_CASIC;
        return true;
    }

    // Not an L76K — try u-blox M10Q default baud rate. It streams NMEA out
    // of the box, so just verify data is flowing.
    SerialGPS.updateBaudRate(38400);
    if (waitForBytes(2000)) {
        Serial.println("[gps] u-blox M10Q detected @38400");
        moduleDetected = true;
        moduleKind = GPS_UBLOX;
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

const char* moduleName() {
    switch (moduleKind) {
        case GPS_CASIC: return "CASIC";
        case GPS_UBLOX: return "u-blox";
        default: return moduleDetected ? "NMEA" : "none";
    }
}

namespace {

// GPS epoch 1980-01-06 = unix 315964800; GPS is ahead of UTC by the leap
// second count (18 since 2017, valid through at least 2025).
constexpr uint32_t GPS_UNIX_EPOCH = 315964800;
constexpr int GPS_UTC_LEAP = 18;

// CASIC AID-INI (class 0x0B id 0x01): position (deg) + optional time seed.
// Frame: BA CE | len(u16) | cls | id | payload[56] | cksum(u32). Little-endian.
void sendCasicAidIni(double lat, double lon, double tow, uint16_t wn,
                     float pAcc, float tAcc, uint8_t flags) {
    uint8_t payload[56] = {0};
    double alt = 0;
    memcpy(payload + 0, &lat, 8);
    memcpy(payload + 8, &lon, 8);
    memcpy(payload + 16, &alt, 8);
    memcpy(payload + 24, &tow, 8);
    memcpy(payload + 36, &pAcc, 4);   // +32 freqBias left 0
    memcpy(payload + 40, &tAcc, 4);   // +44 fAcc, +48 res left 0
    memcpy(payload + 52, &wn, 2);
    payload[54] = 0;                  // timeSource
    payload[55] = flags;

    uint8_t frame[66];
    frame[0] = 0xBA; frame[1] = 0xCE;
    uint16_t len = 56;
    memcpy(frame + 2, &len, 2);
    frame[4] = 0x0B; frame[5] = 0x01;
    memcpy(frame + 6, payload, 56);
    // Checksum: first word = len | (cls<<16) | (id<<24), then each payload word.
    uint32_t ck = (uint32_t)len | ((uint32_t)0x0B << 16) | ((uint32_t)0x01 << 24);
    for (int i = 0; i < 56; i += 4) {
        ck += (uint32_t)payload[i] | ((uint32_t)payload[i + 1] << 8) |
              ((uint32_t)payload[i + 2] << 16) | ((uint32_t)payload[i + 3] << 24);
    }
    memcpy(frame + 62, &ck, 4);
    SerialGPS.write(frame, sizeof(frame));
}

// u-blox UBX frame: B5 62 | cls | id | len(u16) | payload | Fletcher CK_A CK_B.
void sendUbx(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
    uint8_t hdr[6] = {0xB5, 0x62, cls, id, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
    SerialGPS.write(hdr, 6);
    if (len) SerialGPS.write(payload, len);
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 6; ++i) { a += hdr[i]; b += a; }
    for (int i = 0; i < len; ++i) { a += payload[i]; b += a; }
    uint8_t ck[2] = {a, b};
    SerialGPS.write(ck, 2);
}

}  // namespace

void injectAiding(double lat, double lon, time_t utc, bool haveTime,
                  float posAccM, float timeAccS) {
    if (moduleKind == GPS_CASIC) {
        double tow = 0;
        uint16_t wn = 0;
        uint8_t flags = 0x01 | 0x20 | 0x40;   // pos valid | LLA (degrees) | alt invalid
        if (haveTime) {
            uint32_t gs = (uint32_t)(utc - GPS_UNIX_EPOCH) + GPS_UTC_LEAP;
            wn = gs / 604800;
            tow = gs % 604800;
            flags |= 0x02;                    // time valid
        }
        sendCasicAidIni(lat, lon, tow, wn, posAccM, timeAccS, flags);
        Serial.printf("[gps] CASIC AID-INI: %.5f,%.5f time=%d\n", lat, lon, haveTime);
    } else if (moduleKind == GPS_UBLOX) {
        if (haveTime) {                       // MGA-INI-TIME_UTC (type 0x10, len 24)
            struct tm t;
            time_t u = utc;
            gmtime_r(&u, &t);
            uint8_t p[24] = {0};
            p[0] = 0x10; p[2] = 0x00; p[3] = (uint8_t)GPS_UTC_LEAP;
            uint16_t yr = t.tm_year + 1900;
            memcpy(p + 4, &yr, 2);
            p[6] = t.tm_mon + 1; p[7] = t.tm_mday;
            p[8] = t.tm_hour; p[9] = t.tm_min; p[10] = t.tm_sec;
            uint16_t tAccS = (uint16_t)(timeAccS + 0.5f);
            memcpy(p + 16, &tAccS, 2);
            sendUbx(0x13, 0x40, p, 24);       // time before position
        }
        uint8_t p[20] = {0};                  // MGA-INI-POS_LLH (type 0x01, len 20)
        p[0] = 0x01;
        int32_t latE7 = (int32_t)llround(lat * 1e7);
        int32_t lonE7 = (int32_t)llround(lon * 1e7);
        uint32_t accCm = (uint32_t)(posAccM * 100.0f);
        memcpy(p + 4, &latE7, 4);
        memcpy(p + 8, &lonE7, 4);             // +12 alt left 0
        memcpy(p + 16, &accCm, 4);
        sendUbx(0x13, 0x40, p, 20);
        Serial.printf("[gps] u-blox MGA-INI: %.5f,%.5f time=%d\n", lat, lon, haveTime);
    }
}

// Pending phone fix, applied by the GPS task (all UART writes stay on-task).
struct PendingSeed {
    volatile bool pending = false;
    double lat = 0, lon = 0;
    time_t utc = 0;
    bool haveTime = false;
    float posAccM = 0;
};
static PendingSeed g_seed;

void seedPosition(double lat, double lon, time_t utc, bool haveTime,
                  float posAccM) {
    g_seed.lat = lat;
    g_seed.lon = lon;
    g_seed.utc = utc;
    g_seed.haveTime = haveTime;
    g_seed.posAccM = posAccM;
    g_seed.pending = true;   // publish last so the task sees a complete record
}

void task(void*) {
    for (;;) {
        if (g_seed.pending) {
            g_seed.pending = false;
            injectAiding(g_seed.lat, g_seed.lon, g_seed.utc, g_seed.haveTime,
                         g_seed.posAccM, 30.0f);
        }

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

        // Keep the ESP32 system clock in sync with GPS time. It survives deep
        // sleep, so after a shutdown/wake we can seed the receiver with an
        // accurate time (warm start) even before the first fix.
        static uint32_t lastClockSet = 0;
        if (gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2025 &&
            millis() - lastClockSet > 60000) {
            lastClockSet = millis();
            struct timeval tv = {toUnix(gps.date.year(), gps.date.month(),
                                        gps.date.day(), gps.time.hour(),
                                        gps.time.minute(), gps.time.second()), 0};
            settimeofday(&tv, nullptr);
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
