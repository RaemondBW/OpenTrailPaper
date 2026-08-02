#include "gps_service.h"

#include <Arduino.h>
#include <TinyGPS++.h>
#include <freertos/FreeRTOS.h>
#include <esp_heap_caps.h>

#include "config.h"
#include "ride_state.h"
#include "routes.h"
#include "settings.h"
#include "rtc_clock.h"
#include "board_power.h"
#include "diag.h"

#define SerialGPS Serial2

namespace {

TinyGPSPlus gps;
// Satellites in view, per constellation (GSV term 3 repeats the total).
TinyGPSCustom gpgsvInView(gps, "GPGSV", 3);   // GPS
TinyGPSCustom glgsvInView(gps, "GLGSV", 3);   // GLONASS
TinyGPSCustom bdgsvInView(gps, "GBGSV", 3);   // BeiDou (L76K uses GB)
// C/N0 of the up-to-four satellites in each GSV message (terms 7/11/15/19) —
// signal strength is the decisive weak-signal-vs-obstruction metric. Parse it
// for every constellation so bestSnr reflects whatever's strongest overhead.
TinyGPSCustom gpgsvSnr0(gps, "GPGSV", 7);
TinyGPSCustom gpgsvSnr1(gps, "GPGSV", 11);
TinyGPSCustom gpgsvSnr2(gps, "GPGSV", 15);
TinyGPSCustom gpgsvSnr3(gps, "GPGSV", 19);
TinyGPSCustom glgsvSnr0(gps, "GLGSV", 7);
TinyGPSCustom glgsvSnr1(gps, "GLGSV", 11);
TinyGPSCustom glgsvSnr2(gps, "GLGSV", 15);
TinyGPSCustom glgsvSnr3(gps, "GLGSV", 19);
TinyGPSCustom bdgsvSnr0(gps, "GBGSV", 7);
TinyGPSCustom bdgsvSnr1(gps, "GBGSV", 11);
TinyGPSCustom bdgsvSnr2(gps, "GBGSV", 15);
TinyGPSCustom bdgsvSnr3(gps, "GBGSV", 19);
// Fix mode and dilution-of-precision from the combined GSA sentence: term 2 is
// the fix type (1=none, 2=2D, 3=3D), 15/16/17 are P/H/V-DOP. Both talker IDs
// appear in the wild (GN for multi-constellation, GP for GPS-only).
TinyGPSCustom gngsaFix(gps, "GNGSA", 2);
TinyGPSCustom gngsaPdop(gps, "GNGSA", 15);
TinyGPSCustom gngsaVdop(gps, "GNGSA", 17);
TinyGPSCustom gpgsaFix(gps, "GPGSA", 2);
int bestSnr = 0;
bool moduleDetected = false;
enum GpsKind { GPS_NONE, GPS_CASIC, GPS_UBLOX };
GpsKind moduleKind = GPS_NONE;

// Last aiding we injected, surfaced in the serial telemetry so a slow fix can
// be correlated with whether (and how well) the receiver was seeded. posAccM is
// the uncertainty we claimed for it, which is also the radius inside which a
// fresh phone position tells the receiver nothing new (see aidingIsNews).
struct AidState {
    uint32_t count = 0;
    uint32_t skipped = 0;   // phone seeds withheld as redundant
    uint32_t lastMs = 0;
    double lat = 0, lon = 0;
    float posAccM = 0;
    bool haveTime = false;
} aidState;

// Acquisition tracking, so TTFF is measured from each (re)start rather than
// from boot — makes the cold-start iteration loop read cleanly.
volatile int g_coldReq = 0;   // 0 none, 1 cold+aided, 2 cold+unaided
uint32_t acqStartMs = 0;
bool g_loggedFirstFix = false;
bool g_prevFix = false;

// AGNSS ephemeris blob (from the phone/console), buffered whole then sent to the
// receiver one CASIC message at a time, ACK-gated: the module NAKs messages that
// arrive while it's still busy, so back-to-back streaming loses most of them
// (measured: 4 ACK / 30 NAK). Buffer, then send-and-wait-for-ACK per message.
uint8_t* agnssBuf = nullptr;
size_t   agnssLen = 0, agnssCap = 0;
volatile bool agnssReady = false;
constexpr size_t AGNSS_CAP = 32 * 1024;

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

// Compact 1 Hz acquisition telemetry to USB serial, so first-fix behaviour can
// be watched and iterated on live. Serial-only (never the SD log). Everything
// here is read-only against the shared TinyGPS parser, called from the GPS task.
void printSerialTelemetry() {
#if GPS_DEBUG_SERIAL
    bool fix = gps.location.isValid() && gps.location.age() < 3000;
    int inView = (gpgsvInView.isValid() ? atoi(gpgsvInView.value()) : 0) +
                 (glgsvInView.isValid() ? atoi(glgsvInView.value()) : 0) +
                 (bdgsvInView.isValid() ? atoi(bdgsvInView.value()) : 0);
    int fixType = gngsaFix.isValid() && *gngsaFix.value()
                      ? atoi(gngsaFix.value())
                      : (gpgsaFix.isValid() ? atoi(gpgsaFix.value()) : 0);
    float pdop = gngsaPdop.isValid() ? atof(gngsaPdop.value()) : 0.0f;
    float vdop = gngsaVdop.isValid() ? atof(gngsaVdop.value()) : 0.0f;

    time_t sysNow = time(nullptr);
    char sysBuf[16] = "unset";
    if (sysNow > 1735689600) {
        struct tm t; gmtime_r(&sysNow, &t);
        snprintf(sysBuf, sizeof(sysBuf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    }
    char gpsBuf[16] = "--:--:--";
    if (gps.time.isValid())
        snprintf(gpsBuf, sizeof(gpsBuf), "%02d:%02d:%02d",
                 gps.time.hour(), gps.time.minute(), gps.time.second());

    Serial.printf(
        "[gpsdbg t=%lus] %s type=%d sats=%d/%d snr=%d hdop=%.1f pdop=%.1f vdop=%.1f "
        "chars=%lu ck=%lu/%lu wfix=%lu locAge=%ldms sys=%s gps=%s aid=%lux%s",
        (unsigned long)((millis() - acqStartMs) / 1000),
        fix ? "FIX " : "SRCH",
        fixType,
        gps.satellites.isValid() ? (int)gps.satellites.value() : 0, inView,
        bestSnr,
        gps.hdop.isValid() ? gps.hdop.hdop() : 0.0,
        pdop, vdop,
        (unsigned long)gps.charsProcessed(),
        (unsigned long)gps.passedChecksum(), (unsigned long)gps.failedChecksum(),
        (unsigned long)gps.sentencesWithFix(),
        gps.location.isValid() ? (long)gps.location.age() : -1L,
        sysBuf, gpsBuf,
        (unsigned long)aidState.count,
        aidState.count ? (aidState.haveTime ? " pos+time" : " pos") : "");
    if (fix)
        Serial.printf(" @ %.6f,%.6f alt=%.0f spd=%.1f",
                      gps.location.lat(), gps.location.lng(),
                      gps.altitude.isValid() ? gps.altitude.meters() : 0.0,
                      gps.speed.isValid() ? gps.speed.kmph() : 0.0);
    Serial.println();
#endif
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
    // Bigger RX ring than the 256-byte default: with GPS+GLONASS+BeiDou all
    // streaming NMEA, a burst of GSV sentences can otherwise overrun the buffer
    // between task polls and drop bytes mid-sentence — a dropped GGA/RMC costs a
    // whole second of fix confirmation. Must be set before begin().
    SerialGPS.setRxBufferSize(1024);
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

void logBanner() {
#if GPS_DEBUG_SERIAL
    Serial.printf("[gpsdbg] --- GPS telemetry on. module=%s rxbuf=1024 "
                  "echo=%d ---\n", moduleName(), GPS_ECHO_NMEA);
    Serial.println("[gpsdbg] legend: type(0none/2=2D/3=3D) sats=inUse/inView "
                   "snr=best C/N0 dB-Hz  ck=ok/bad  aid=count(pos|pos+time)");
#endif
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
    aidState.count++;
    aidState.lastMs = millis();
    aidState.lat = lat;
    aidState.lon = lon;
    aidState.posAccM = posAccM;
    aidState.haveTime = haveTime;
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
        diag::log("gps aiding (CASIC): %.4f,%.4f time=%d", lat, lon, haveTime);
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
        diag::log("gps aiding (u-blox): %.4f,%.4f time=%d", lat, lon, haveTime);
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

// Floor between two position-only re-seeds. Only guards the "we moved" test
// below; a seed carrying strictly better information is never delayed by it.
constexpr uint32_t AID_REPEAT_MIN_MS = 60000;

// Does a phone position tell the receiver anything it wasn't already told?
//
// AID-INI is a one-shot acquisition aid, not a subscription: every injection
// restarts the receiver's search. The phone streams its location every ~3 s, so
// a plain rate-limit still let 48 injections through in one 5 h session (diag
// log 2026-07-31) — the *same* coordinates re-sent every 20 s for 40 minutes,
// at a receiver that never fixed, and whose one first fix took 716 s. That is
// not neutral: each re-seed throws away the correlation work in progress.
//
// So accept a seed only when it is genuinely better information than the last
// one we sent — otherwise the best thing we can do for first-fix time is shut
// up and let the receiver work.
static bool aidingIsNews(const PendingSeed& s) {
    if (aidState.count == 0) return true;              // nothing sent yet
    if (s.haveTime && !aidState.haveTime) return true; // time bounds the search
    // The boot seed is the saved NVS position at 50 km; a phone CoreLocation
    // fix is 5 km. Tightening the uncertainty by 2x is worth one re-send.
    if (s.posAccM < aidState.posAccM * 0.5f) return true;
    // Position-only change: we have to be outside the circle we already gave
    // the receiver for this to mean anything, and the input jitters.
    if (millis() - aidState.lastMs < AID_REPEAT_MIN_MS) return false;
    double dLat = s.lat - aidState.lat;
    double dLon = (s.lon - aidState.lon) * cos(aidState.lat * DEG_TO_RAD);
    return sqrt(dLat * dLat + dLon * dLon) * 111320.0 > aidState.posAccM;
}

void seedPosition(double lat, double lon, time_t utc, bool haveTime,
                  float posAccM) {
    g_seed.lat = lat;
    g_seed.lon = lon;
    g_seed.utc = utc;
    g_seed.haveTime = haveTime;
    g_seed.posAccM = posAccM;
    g_seed.pending = true;   // publish last so the task sees a complete record
}

void forceColdStart(bool withAiding) { g_coldReq = withAiding ? 1 : 2; }

int moduleKindCode() { return (int)moduleKind; }   // 0 none, 1 CASIC, 2 u-blox

// Console-driven investigation flags, serviced on the GPS task.
volatile bool g_rawEcho = false;
volatile bool g_verReq = false;
volatile int  g_powerCycleMs = 0;

void setRawEcho(bool on) { g_rawEcho = on; }
void queryVersion() { g_verReq = true; }
void powerCycleTest(int offMs) { g_powerCycleMs = offMs > 0 ? offMs : 1; }

char g_cmdBuf[64];
volatile bool g_cmdReq = false;
void sendNmeaCommand(const char* body) {
    strncpy(g_cmdBuf, body, sizeof(g_cmdBuf) - 1);
    g_cmdBuf[sizeof(g_cmdBuf) - 1] = 0;
    g_cmdReq = true;
}

// Re-seed the receiver exactly like boot does: last-known position, plus time
// only if the RTC has been GPS-validated. Shared by the cold-start test path.
void seedFromSaved() {
    double alat, alon;
    if (!settings::lastPosition(alat, alon)) return;
    time_t now = settings::rtcTrusted() ? time(nullptr) : 0;
    bool haveTime = now > 1735689600;
    injectAiding(alat, alon, now, haveTime, 50000.0f, 30.0f);
}

void agnssBegin() {
    if (!agnssBuf) agnssBuf = (uint8_t*)heap_caps_malloc(AGNSS_CAP, MALLOC_CAP_SPIRAM);
    agnssLen = 0;
    agnssReady = false;
    seedFromSaved();          // position/time first, so ephemeris is bounded
    diag::log("agnss: injection begin");
}

void agnssInject(const uint8_t* data, size_t len) {
    if (!agnssBuf || !len) return;
    if (agnssLen + len > AGNSS_CAP) len = AGNSS_CAP - agnssLen;
    memcpy(agnssBuf + agnssLen, data, len);
    agnssLen += len;
}

void agnssEnd() {
    agnssReady = true;        // GPS task sends it, ACK-gated, then re-seeds time
    diag::log("agnss: %u bytes buffered, sending", (unsigned)agnssLen);
}

// Wait up to timeoutMs for a CASIC ACK (BA CE .. 05 01) or NAK (.. 05 00),
// keeping NMEA parsing alive. Returns 1 ACK, 0 NAK, -1 timeout.
int waitForCasicAck(uint32_t timeoutMs) {
    uint32_t start = millis();
    int st = 0;   // scan for BA CE <lenlo> <lenhi> <cls=05> <id>
    while (millis() - start < timeoutMs) {
        while (SerialGPS.available()) {
            uint8_t c = SerialGPS.read();
            gps.encode(c);
            switch (st) {
                case 0: st = (c == 0xBA) ? 1 : 0; break;
                case 1: st = (c == 0xCE) ? 2 : (c == 0xBA ? 1 : 0); break;
                case 2: st = 3; break;              // len lo
                case 3: st = 4; break;              // len hi
                case 4: st = (c == 0x05) ? 5 : (c == 0xBA ? 1 : 0); break;   // class
                case 5:
                    if (c == 0x01) return 1;        // ACK
                    if (c == 0x00) return 0;        // NAK
                    st = (c == 0xBA) ? 1 : 0; break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return -1;
}

// Send the buffered blob one CASIC message at a time, waiting for each ACK.
// Frame = BA CE | len(u16) | cls | id | payload[len] | cksum(u32) = 10+len.
void sendAgnssGated() {
    int ok = 0, nak = 0, to = 0, msgs = 0;
    size_t i = 0;
    while (i + 6 <= agnssLen) {
        if (agnssBuf[i] != 0xBA || agnssBuf[i + 1] != 0xCE) { i++; continue; }
        uint16_t len = agnssBuf[i + 2] | ((uint16_t)agnssBuf[i + 3] << 8);
        size_t frame = 10 + len;
        if (i + frame > agnssLen) break;
        SerialGPS.write(agnssBuf + i, frame);
        int r = waitForCasicAck(300);
        if (r == 1) ok++; else if (r == 0) nak++; else to++;
        msgs++;
        i += frame;
    }
    diag::log("agnss gated: %d msgs, %d ACK %d NAK %d timeout", msgs, ok, nak, to);
    Serial.printf("[agnss] %d msgs: %d ACK, %d NAK, %d timeout\n", msgs, ok, nak, to);
    seedFromSaved();          // re-seed time so the fresh ephemeris applies now
}

// Tell the receiver to cold-start: forget ephemeris, almanac, last position and
// time. This is the honest way to measure first-fix time — a brief power cut
// doesn't clear the module's backup-powered RAM, so it hot-starts in a few sec.
void sendColdStartCommand() {
    if (moduleKind == GPS_CASIC) {
        SerialGPS.write("$PCAS10,2*1E\r\n");     // 2 = cold start
    } else if (moduleKind == GPS_UBLOX) {
        uint8_t p[4] = {0xFF, 0xFF, 0x02, 0x00}; // navBbrMask=cold, sw reset
        sendUbx(0x06, 0x04, p, 4);               // CFG-RST
    }
}

void task(void*) {
    logBanner();
    acqStartMs = millis();
    for (;;) {
        // On-demand cold-start test for iteration: wipe the receiver's stored
        // ephemeris/almanac/time/position, optionally re-seed, and reset the
        // TTFF clock — mirrors a real cold wake after a long power-off.
        if (g_coldReq) {
            int mode = g_coldReq;
            g_coldReq = 0;
            diag::log("gps cold-start test: %s", mode == 1 ? "AIDED" : "unaided");
            sendColdStartCommand();
            vTaskDelay(pdMS_TO_TICKS(600));   // let the cold start take effect
            aidState.count = 0;               // TTFF context reflects THIS test
            aidState.skipped = 0;
            if (mode == 1) seedFromSaved();
            acqStartMs = millis();
            g_loggedFirstFix = false;
            g_prevFix = false;
        }

        if (g_verReq) {
            g_verReq = false;
            // PCAS06,0 -> the module replies with a $GPTXT version line. Echo
            // raw for a moment so it lands on the console verbatim.
            g_rawEcho = true;
            SerialGPS.write("$PCAS06,0*1B\r\n");
        }

        if (g_cmdReq) {
            g_cmdReq = false;
            uint8_t ck = 0;
            for (const char* p = g_cmdBuf; *p; ++p) ck ^= (uint8_t)*p;
            g_rawEcho = true;
            SerialGPS.printf("$%s*%02X\r\n", g_cmdBuf, ck);
        }

        if (g_powerCycleMs > 0) {
            int off = g_powerCycleMs;
            g_powerCycleMs = 0;
            diag::log("gps power-cycle test: off %dms", off);
            board_radio_power(false);
            vTaskDelay(pdMS_TO_TICKS(off));
            board_radio_power(true);
            vTaskDelay(pdMS_TO_TICKS(400));
            begin();
            aidState.count = 0;
            aidState.skipped = 0;
            seedFromSaved();
            acqStartMs = millis();
            g_loggedFirstFix = false;
            g_prevFix = false;
        }

        if (g_seed.pending) {
            g_seed.pending = false;
            // Never re-seed a receiver that already has a fix, and otherwise
            // only when the phone is telling it something new (aidingIsNews).
            bool haveFix = gps.location.isValid() && gps.location.age() < 5000;
            if (!haveFix && aidingIsNews(g_seed)) {
                injectAiding(g_seed.lat, g_seed.lon, g_seed.utc, g_seed.haveTime,
                             g_seed.posAccM, 30.0f);
            } else {
                aidState.skipped++;
            }
        }

        while (SerialGPS.available()) {
            char c = SerialGPS.read();
#if GPS_ECHO_NMEA
            Serial.write(c);
#else
            if (g_rawEcho) Serial.write(c);
#endif
            gps.encode(c);
        }

        // A completed AGNSS blob is sent here, ACK-gated (blocks this loop for
        // the ~seconds of injection, which is fine — we're not fixing meanwhile).
        if (agnssReady) {
            agnssReady = false;
            sendAgnssGated();
        }

        // Track the strongest C/N0 across all constellations' GSV batches;
        // recompute whenever a fresh GPGSV batch arrives (roughly 1 Hz).
        if (gpgsvSnr0.isUpdated()) {
            TinyGPSCustom* snrs[12] = {&gpgsvSnr0, &gpgsvSnr1, &gpgsvSnr2, &gpgsvSnr3,
                                       &glgsvSnr0, &glgsvSnr1, &glgsvSnr2, &glgsvSnr3,
                                       &bdgsvSnr0, &bdgsvSnr1, &bdgsvSnr2, &bdgsvSnr3};
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

        // Persist the position so the map — and the next boot's warm-start
        // seed — start from the last known location. Save the very first fix
        // immediately (so a short session still leaves a fresh seed), then at
        // most every 2 min. Also saved on shutdown.
        static uint32_t lastPosSave = 0;
        static bool savedFirstFix = false;
        if (gps.location.isValid() &&
            (!savedFirstFix || millis() - lastPosSave > 120000)) {
            savedFirstFix = true;
            lastPosSave = millis();
            settings::setLastPosition(gps.location.lat(), gps.location.lng());
        }

        // Keep the ESP32 system clock in sync with GPS time. It survives deep
        // sleep, so after a shutdown/wake we can seed the receiver with an
        // accurate time (warm start) even before the first fix.
        static uint32_t lastClockSet = 0;
        static uint32_t lastRtcWrite = 0;
        if (gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2025 &&
            millis() - lastClockSet > 60000) {
            lastClockSet = millis();
            time_t u = toUnix(gps.date.year(), gps.date.month(), gps.date.day(),
                              gps.time.hour(), gps.time.minute(), gps.time.second());
            struct timeval tv = {u, 0};
            settimeofday(&tv, nullptr);
            // Also push GPS time into the coin-cell RTC (every ~10 min), so it
            // stays accurate across a full power-off and can seed time-aiding
            // on the next cold boot.
            if (lastRtcWrite == 0 || millis() - lastRtcWrite > 600000) {
                lastRtcWrite = millis();
                rtc_clock::write(u);
                // The RTC now holds GPS-sourced UTC, so it's safe to seed
                // time-aiding from it on future boots.
                settings::setRtcTrusted(true);
            }
        }

        // GPS acquisition diagnostics to the SD log, so a "won't get a fix"
        // problem is diagnosable afterward. Reads like: chars=NMEA bytes (0 =
        // module silent → power/wiring/baud), ck=good/bad checksums (data
        // quality), sats=inUse/inView (0 in view → no sky/antenna), snr=best
        // C/N0 dB-Hz (low → weak signal / indoors), hdop=geometry. Logged more
        // often while searching, plus first-fix time and fix gain/loss.
        {
            static uint32_t lastGpsLog = 0;
            static bool loggedModule = false;
            if (!loggedModule) {
                loggedModule = true;
                diag::log("gps module: %s", moduleName());
            }
            bool haveFix = gps.location.isValid() && gps.location.age() < 3000;
            uint32_t interval = haveFix ? 120000 : 15000;
            if (millis() - lastGpsLog > interval) {
                lastGpsLog = millis();
                GpsDebug d;
                getDebug(d);
                diag::log("gps %s: chars=%lu ck=%lu/%lu sats=%d/%d snr=%d hdop=%.1f",
                          haveFix ? "FIX" : "searching", (unsigned long)d.chars,
                          (unsigned long)d.passedCksum, (unsigned long)d.failedCksum,
                          d.satsInUse, d.satsInView, d.bestSnr, d.hdop);
            }
            if (haveFix != g_prevFix) {
                g_prevFix = haveFix;
                if (haveFix && !g_loggedFirstFix) {
                    g_loggedFirstFix = true;
                    // TTFF measured from this acquisition start (boot or the
                    // last reacquire), with the seeding context that explains
                    // it — the key variable when iterating on fix speed.
                    diag::log("gps FIRST FIX in %lus (sats=%d snr=%d hdop=%.1f "
                              "aided=%lux%s, %lu redundant seeds withheld)",
                              (unsigned long)((millis() - acqStartMs) / 1000),
                              gps.satellites.isValid() ? (int)gps.satellites.value() : 0,
                              bestSnr, gps.hdop.isValid() ? gps.hdop.hdop() : 0.0,
                              (unsigned long)aidState.count,
                              aidState.count ? (aidState.haveTime ? " pos+time"
                                                                  : " pos") : " none",
                              (unsigned long)aidState.skipped);
                } else {
                    diag::log("gps: fix %s", haveFix ? "reacquired" : "LOST");
                }
            }
        }

        // High-rate serial telemetry for live iteration: 1 Hz while searching,
        // 5 Hz-slow (5 s) once locked so the console isn't a firehose.
        {
            static uint32_t lastTelem = 0;
            bool haveFix = gps.location.isValid() && gps.location.age() < 3000;
            uint32_t telemInterval = haveFix ? 5000 : 1000;
            if (millis() - lastTelem > telemInterval) {
                lastTelem = millis();
                printSerialTelemetry();
            }
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
