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
// Mesh messaging, OFF until the rider turns it on. Joining a public mesh means
// announcing yourself on it and spending battery listening, and neither should
// happen because a firmware update landed — it is a choice, so the device waits to
// be asked. The Mesh tab has the switch.
bool meshOn = false;
// "" = no explicit channel; mesh_service derives the name from the modem preset.
char meshChan[16] = "";
uint8_t meshKey = 1;
// Modem preset index into mesh::kPresets. Unlike the channel name this IS stored
// as a plain value: it is a user choice with no compile-time twin to follow.
uint8_t meshPresetIdx = MESH_PRESET_DEFAULT;
// Bitmask of channels we share our position on. 0 = none, which is the default:
// telling a public mesh where you are should be a decision, not an accident.
uint8_t meshPosMask = 0;
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
    meshOn = prefs.getBool("meshon", false);
    prefs.getString("meshchan", meshChan, sizeof(meshChan));   // "" = use the preset
    meshKey = (uint8_t)constrain(prefs.getUChar("meshkey", 1), 1, 10);
    meshPresetIdx = prefs.getUChar("meshpreset", MESH_PRESET_DEFAULT);
    meshPosMask = prefs.getUChar("meshposch", 0);
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
    // An EMPTY name is a real value here — it means "no pinned channel, follow the
    // modem preset" — so it must be STORED, not rejected. This guard existed in
    // three layers (here, mesh_service::setChannel and applyChannelChange) and
    // this was the last one: clearing the channel worked in RAM and silently came
    // back on the next boot, because the empty string never reached NVS.
    if (!name) return;
    snprintf(meshChan, sizeof(meshChan), "%s", name);
    meshKey = (uint8_t)constrain((int)keyIndex, 1, 10);
    prefs.putString("meshchan", meshChan);
    prefs.putUChar("meshkey", meshKey);
}

size_t meshPrivateChannels(uint8_t* out, size_t cap) {
    return prefs.getBytes("meshchans", out, cap);
}

void setMeshPrivateChannels(const uint8_t* data, size_t len) {
    // An empty set removes the key rather than storing zero bytes, so a device
    // with no private channels reads back cleanly.
    if (!data || len == 0) prefs.remove("meshchans");
    else prefs.putBytes("meshchans", data, len);
}

uint8_t meshPositionChannels() { return meshPosMask; }
void setMeshPositionChannels(uint8_t mask) {
    if (meshPosMask == mask) return;
    meshPosMask = mask;
    prefs.putUChar("meshposch", mask);
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
