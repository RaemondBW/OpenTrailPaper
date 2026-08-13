#include "settings.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cmath>

#include "config.h"
#include "mesh_proto.h"

namespace {

Preferences prefs;
float compassOff = NAN;
int ftp = FTP_WATTS;
int tz = TIMEZONE_OFFSET_MINUTES;
int bl = 2;  // backlight level 0-3
bool miles = false;  // false = km, true = miles
bool clk24 = true;   // true = 24-hour clock, false = 12-hour
bool usbDrv = true;  // true = expose SD as USB drive when plugged into a host
// false = a field whose sensor is unpaired is dropped from the dashboard and
// the rest re-pack; true = it stays, showing no data.
bool showOff = false;
char addrs[3][18] = {"", "", ""};
char names[3][32] = {"", "", ""};   // remembered vendor/model per paired kind
double lastLat = 0, lastLon = 0;
bool rtcSynced = false;  // has GPS ever written UTC to the coin-cell RTC?
// Mesh messaging. Default ON with the default channel, which is what makes a
// device out of the box able to hear the public mesh around it.
bool meshOn = true;
// "" = no explicit channel; mesh_service derives the name from the modem preset.
char meshChan[16] = "";
uint8_t meshKey = 1;
// Modem preset index into mesh::kPresets. Unlike the channel name this IS stored
// as a plain value: it is a user choice with no compile-time twin to follow.
uint8_t meshPresetIdx = MESH_PRESET_DEFAULT;
char meshLong[40] = "";
char meshShort[8] = "";
const char* KEYS[3] = {"sens_hr", "sens_pwr", "sens_cad"};
const char* NAME_KEYS[3] = {"snm_hr", "snm_pwr", "snm_cad"};

}  // namespace

namespace settings {

void begin() {
    prefs.begin("bike", false);
    compassOff = prefs.getFloat("cmpoff", NAN);
    ftp = prefs.getInt("ftp", FTP_WATTS);
    tz = prefs.getInt("tz", TIMEZONE_OFFSET_MINUTES);
    bl = prefs.getInt("bl", 2);
    miles = prefs.getBool("miles", false);
    clk24 = prefs.getBool("clk24", true);
    // Key bumped to usbdrv2 so the new OFF default takes effect even on devices
    // that had the old (default-ON) "usbdrv" value stored. OFF = the device
    // keeps the SD (logs/recording) when plugged in.
    usbDrv = prefs.getBool("usbdrv2", false);
    showOff = prefs.getBool("showoff", false);
    for (int k = 0; k < 3; ++k) {
        prefs.getString(KEYS[k], addrs[k], sizeof(addrs[k]));
        prefs.getString(NAME_KEYS[k], names[k], sizeof(names[k]));
    }
    lastLat = prefs.getDouble("lastlat", 0);
    lastLon = prefs.getDouble("lastlon", 0);
    rtcSynced = prefs.getBool("rtcok", false);
    meshOn = prefs.getBool("meshon", true);
    prefs.getString("meshchan", meshChan, sizeof(meshChan));   // "" = use the preset
    meshKey = (uint8_t)constrain(prefs.getUChar("meshkey", 1), 1, 10);
    meshPresetIdx = prefs.getUChar("meshpreset", MESH_PRESET_DEFAULT);
    // A preset written by a newer firmware that knew more of them must not leave
    // this build driving the radio with garbage.
    if (meshPresetIdx >= mesh::PRESET_COUNT) meshPresetIdx = MESH_PRESET_DEFAULT;
    prefs.getString("meshlong", meshLong, sizeof(meshLong));
    prefs.getString("meshshort", meshShort, sizeof(meshShort));
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

bool usbDrive() { return usbDrv; }
void setUsbDrive(bool on) {
    usbDrv = on;
    prefs.putBool("usbdrv2", usbDrv);
}

bool showOffline() { return showOff; }
void setShowOffline(bool on) {
    showOff = on;
    prefs.putBool("showoff", showOff);
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

float compassOffsetDeg() { return compassOff; }
void setCompassOffsetDeg(float deg) {
    // Written rarely on purpose — NVS has a finite erase budget and this is a
    // slowly-learned constant, not a live value. aux_sensors only calls this
    // when the learned offset has actually drifted a few degrees.
    compassOff = deg;
    prefs.putFloat("cmpoff", deg);
}

bool meshEnabled() { return meshOn; }
void setMeshEnabled(bool on) {
    if (meshOn == on) return;
    meshOn = on;
    prefs.putBool("meshon", on);
}

// "" means the rider has chosen no channel name, and mesh_service then derives it
// from the modem preset the way a stock Meshtastic node does. Deliberately NOT
// defaulted to a fixed string here: pinning a name would keep a device on
// LongFast's frequency slot after its preset moved to MediumFast, which no stock
// node would be listening on.
const char* meshChannel() { return meshChan; }
uint8_t meshChannelKey() { return meshKey; }

void setMeshChannel(const char* name, uint8_t keyIndex) {
    if (!name || !name[0]) return;
    snprintf(meshChan, sizeof(meshChan), "%s", name);
    meshKey = (uint8_t)constrain((int)keyIndex, 1, 10);
    prefs.putString("meshchan", meshChan);
    prefs.putUChar("meshkey", meshKey);
}

uint8_t meshPreset() { return meshPresetIdx; }
void setMeshPreset(uint8_t index) {
    if (index >= mesh::PRESET_COUNT) return;
    meshPresetIdx = index;
    prefs.putUChar("meshpreset", index);
}

const char* meshLongName() { return meshLong; }
const char* meshShortName() { return meshShort; }

void setMeshNames(const char* longName, const char* shortName) {
    if (longName && longName[0]) {
        snprintf(meshLong, sizeof(meshLong), "%s", longName);
        prefs.putString("meshlong", meshLong);
    }
    if (shortName && shortName[0]) {
        snprintf(meshShort, sizeof(meshShort), "%s", shortName);
        prefs.putString("meshshort", meshShort);
    }
}

bool rtcTrusted() { return rtcSynced; }
void setRtcTrusted(bool ok) {
    if (rtcSynced == ok) return;
    rtcSynced = ok;
    prefs.putBool("rtcok", ok);
}

}  // namespace settings
