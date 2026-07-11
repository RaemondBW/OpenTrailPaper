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

// kind: 0 HR, 1 Power, 2 Cadence (matches ble_sensors). "" = none saved.
const char* sensorAddr(int kind);
void setSensorAddr(int kind, const char* addr);

// Last known GPS position (map center across reboots). Returns false if
// no position has ever been saved.
bool lastPosition(double& lat, double& lon);
void setLastPosition(double lat, double lon);

}
