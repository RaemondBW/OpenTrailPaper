#pragma once

// NVS-backed persistent settings: rider config + paired sensor addresses.
// config.h values are the first-boot defaults.

#include <cstddef>
#include <cstdint>

namespace settings {

void begin();

int ftpWatts();
// Structured workout: hold at every block boundary until resumed.
bool workoutPauseEachBlock();
void setWorkoutPauseEachBlock(bool on);
void setFtpWatts(int w);

// Auto-pause: seconds of no movement before the ride timer stops counting.
// 0 = auto-pause off. Movement is judged by the recorder from power, cadence,
// wheel rotation and GPS speed — see ride_recorder.cpp.
int autoPauseSec();
void setAutoPauseSec(int s);

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

// --- Meshtastic mesh messaging (see mesh_service.h) ------------------------
// The region is NOT here: it is a compile-time constant in config.h, because
// which band the radio may use is a legal question, not a preference.

bool meshEnabled();               // false = the LoRa radio is never powered up
void setMeshEnabled(bool on);

// Channel name. Feeds both the channel hash and — via a second hash — the
// frequency slot, so two devices must agree on it exactly to hear each other.
const char* meshChannel();
// Which of Meshtastic's ten well-known channel keys, 1-10. 1 is the default
// channel's ("AQ==").
uint8_t meshChannelKey();
void setMeshChannel(const char* name, uint8_t keyIndex);

// The private channels, as meshtastic.ChannelSet bytes — the same encoding a
// share QR carries, so a channel has one representation rather than two.
// Returns the length written, 0 if none stored.
size_t meshPrivateChannels(uint8_t* out, size_t cap);
void setMeshPrivateChannels(const uint8_t* data, size_t len);

// Which channels we broadcast our own position on, as a bitmask over channel
// slots. 0 (the default) means we tell the mesh nothing about where we are.
uint8_t meshPositionChannels();
void setMeshPositionChannels(uint8_t mask);

// Modem preset: an index into mesh::kPresets, deciding how fast the radio talks.
// Separate from the channel, which decides where. MESH_PRESET_DEFAULT until set.
uint8_t meshPreset();
void setMeshPreset(uint8_t index);

// How this node introduces itself on the mesh. Empty until first set, at which
// point mesh_service derives a default from the node number.
const char* meshLongName();
const char* meshShortName();      // up to 4 glyphs, as Meshtastic apps show it
void setMeshNames(const char* longName, const char* shortName);

// Compass mounting yaw, in degrees, learned from GPS course while riding (see
// aux_math::HeadingOffset). Persisted because the board does not move between
// rides, so re-learning it every time would leave the compass wrong for the
// first minutes of every ride. NAN when nothing has been learned yet.
float compassOffsetDeg();
void setCompassOffsetDeg(float deg);

}
