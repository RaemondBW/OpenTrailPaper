#pragma once

// NVS-backed persistent settings: rider config + paired sensor addresses.
// config.h values are the first-boot defaults.

namespace settings {

void begin();

int ftpWatts();
void setFtpWatts(int w);

int tzMinutes();
void setTzMinutes(int m);

int backlight();          // 0=off .. 3=bright
void setBacklight(int b);

bool useMiles();          // false = metric (km), true = imperial (mi/ft/mph)
void setUseMiles(bool m);

bool clock24h();          // true = 24-hour clock, false = 12-hour (AM/PM)
void setClock24h(bool h);

bool usbDrive();          // true = expose SD as a USB drive when plugged in
void setUsbDrive(bool on);

// true = keep a field on the dashboard even when its sensor is not paired,
// showing "no data" rather than removing it and re-packing the layout.
bool showOffline();
void setShowOffline(bool on);

// kind: 0 HR, 1 Power, 2 Cadence (matches ble_sensors). "" = none saved.
const char* sensorAddr(int kind);
void setSensorAddr(int kind, const char* addr);

// Remembered display name (vendor/model) per paired kind, so a paired sensor
// shows its identity even before it reconnects and across reboots.
const char* sensorName(int kind);
void setSensorName(int kind, const char* name);

// Last known GPS position (map center across reboots). Returns false if
// no position has ever been saved.
bool lastPosition(double& lat, double& lon);
void setLastPosition(double lat, double lon);

// Whether GPS has ever written UTC to the coin-cell RTC on this device. Until
// it has, the RTC may hold factory/local time (seen 8 h off UTC), so we must
// NOT seed GPS time-aiding from it — a grossly wrong time slows acquisition.
bool rtcTrusted();
void setRtcTrusted(bool ok);

// Compass mounting yaw, in degrees, learned from GPS course while riding (see
// aux_math::HeadingOffset). Persisted because the board does not move between
// rides, so re-learning it every time would leave the compass wrong for the
// first minutes of every ride. NAN when nothing has been learned yet.
float compassOffsetDeg();
void setCompassOffsetDeg(float deg);

}
