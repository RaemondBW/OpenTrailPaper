#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>

#include "config.h"

namespace {

Preferences prefs;
int ftp = FTP_WATTS;
int tz = TIMEZONE_OFFSET_MINUTES;
int bl = 2;  // frontlight level 0-3
bool miles = false;  // false = km, true = miles
bool clk24 = true;   // true = 24-hour clock, false = 12-hour
char addrs[3][18] = {"", "", ""};
char names[3][32] = {"", "", ""};   // remembered vendor/model per paired kind
double lastLat = 0, lastLon = 0;
const char* KEYS[3] = {"sens_hr", "sens_pwr", "sens_cad"};
const char* NAME_KEYS[3] = {"snm_hr", "snm_pwr", "snm_cad"};

}  // namespace

namespace settings {

void begin() {
    prefs.begin("bike", false);
    ftp = prefs.getInt("ftp", FTP_WATTS);
    tz = prefs.getInt("tz", TIMEZONE_OFFSET_MINUTES);
    bl = prefs.getInt("bl", 2);
    miles = prefs.getBool("miles", false);
    clk24 = prefs.getBool("clk24", true);
    for (int k = 0; k < 3; ++k) {
        prefs.getString(KEYS[k], addrs[k], sizeof(addrs[k]));
        prefs.getString(NAME_KEYS[k], names[k], sizeof(names[k]));
    }
    lastLat = prefs.getDouble("lastlat", 0);
    lastLon = prefs.getDouble("lastlon", 0);
    Serial.printf("[cfg] ftp=%dW tz=%dmin sensors=[%s|%s|%s]\n", ftp, tz,
                  addrs[0], addrs[1], addrs[2]);
}

int ftpWatts() { return ftp; }
void setFtpWatts(int w) {
    ftp = constrain(w, 50, 500);
    prefs.putInt("ftp", ftp);
}

int tzMinutes() { return tz; }
void setTzMinutes(int m) {
    tz = constrain(m, -12 * 60, 14 * 60);
    prefs.putInt("tz", tz);
}

int backlight() { return bl; }
void setBacklight(int b) {
    bl = constrain(b, 0, 3);
    prefs.putInt("bl", bl);
}

bool useMiles() { return miles; }
void setUseMiles(bool m) {
    miles = m;
    prefs.putBool("miles", miles);
}

bool clock24h() { return clk24; }
void setClock24h(bool h) {
    clk24 = h;
    prefs.putBool("clk24", clk24);
}

const char* sensorAddr(int kind) {
    return (kind >= 0 && kind < 3) ? addrs[kind] : "";
}

void setSensorAddr(int kind, const char* addr) {
    if (kind < 0 || kind >= 3) return;
    snprintf(addrs[kind], sizeof(addrs[kind]), "%s", addr ? addr : "");
    prefs.putString(KEYS[kind], addrs[kind]);
}

const char* sensorName(int kind) {
    return (kind >= 0 && kind < 3) ? names[kind] : "";
}

void setSensorName(int kind, const char* name) {
    if (kind < 0 || kind >= 3 || !name || !name[0]) return;
    snprintf(names[kind], sizeof(names[kind]), "%s", name);
    prefs.putString(NAME_KEYS[kind], names[kind]);
}

bool lastPosition(double& lat, double& lon) {
    if (lastLat == 0 && lastLon == 0) return false;
    lat = lastLat;
    lon = lastLon;
    return true;
}

void setLastPosition(double lat, double lon) {
    lastLat = lat;
    lastLon = lon;
    prefs.putDouble("lastlat", lat);
    prefs.putDouble("lastlon", lon);
}

}  // namespace settings
