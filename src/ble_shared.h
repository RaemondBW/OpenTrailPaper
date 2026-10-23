#pragma once

// What the legacy GATT server (ble_server.cpp) and the phone link (link.cpp)
// share: the phone connection's state, and the one function per feature that
// does the work. The two paths differ only in how a request arrives and how
// the answer is encoded (docs/ble-link.md, "Firmware structure").
//
// Everything here is implemented in ble_server.cpp and is safe to call from
// the link task. Functions that touch the SD card take the SD lock
// themselves; callers still respect the legacy gating (ble_shared::sdFree,
// usb_storage::hostActive) before calling them.

#include <stddef.h>
#include <stdint.h>

#include "ble_server.h"

namespace ble_shared {

// --- The phone connection -------------------------------------------------
bool phoneAdmitted();     // the admitted phone is on the link (phoneConnected)
uint16_t phoneHandle();   // its connection handle (0xFFFF none)
uint16_t mtu();           // negotiated ATT MTU
void noteBulk();          // keep the connection interval fast (transfers only)
// SD writers wait while the recorder or a USB host owns the card.
bool sdFree();

// --- Settings -------------------------------------------------------------
// Applies only the fields marked present. FTP, timezone and backlight are
// clamped by settings::; an out-of-range wheel size is refused (kept). The
// USB-drive switch takes effect at once.
struct SettingsPatch {
    bool hasFtp = false;       int ftpW = 0;
    bool hasTz = false;        int tzMin = 0;
    bool hasMiles = false;     bool useMiles = false;
    bool hasBacklight = false; int backlight = 0;
    bool hasClock24 = false;   bool clock24h = false;
    bool hasUsbDrive = false;  bool usbDrive = false;
    bool hasWheel = false;     int wheelMm = 0;
};
void applySettings(const SettingsPatch& p);

// --- The phone's location (warm start, phone fallback fix, speed source) --
struct PhoneFix {
    int32_t latE7 = 0, lonE7 = 0;
    bool haveUtc = false;  uint32_t utc = 0;      // valid only past 2025-01-01
    bool haveAlt = false;  int16_t altM = 0;
    int16_t accM = 0;                             // <= 0 unknown
    float speedMs = __builtin_nanf("");           // NaN = not reported
    float courseDeg = __builtin_nanf("");
};
void applyPhoneLocation(const PhoneFix& f);

// --- Firmware update ------------------------------------------------------
bool legacyUpdateBusy();   // a legacy OTA transfer or install is running
// The link's update, for the on-device "Updating firmware" popup and the
// interval policy: phase 0 idle, 1 receiving, 2 installing.
void setLinkUpdate(int phase, uint32_t got, uint32_t total);
// Writes the image to /firmware.bin (8 KB chunks, yielding). The boot path
// flashes it after rebootForUpdate().
bool saveFirmwareImage(const uint8_t* img, size_t len);
void rebootForUpdate();    // the server task restarts ~1.5 s later

// --- Files ----------------------------------------------------------------
enum class Dir { Rides, Logs, Routes, Workouts };
// Every regular file in the directory, with the legacy filters (logs: names
// containing ".log"; workouts: no dotfiles). Runs under the SD lock: `cb`
// must be quick. False when the directory can't be opened.
bool listDir(Dir d, void (*cb)(const char* name, uint32_t size, void* ctx), void* ctx);
bool deleteRide(const char* name);
bool deleteRoute(const char* name);      // also drops the active route of that name
bool deleteWorkout(const char* name);    // and refreshes the device's picker
enum class WorkoutSave { Ok, IoError, LoadFailed };
// Saves /workouts/<name>, then loads it. `reason` gets the load failure.
WorkoutSave saveWorkout(const char* name, const char* data, size_t len, const char** reason);

// --- Map tiles: the ids (and version records) on the card ------------------
// Walks /maps/tiles once into PSRAM tables shared with the legacy list; hold
// the lock until done with them. vers == nullptr: no version records (asked
// without, or out of PSRAM). False: no PSRAM at all (lock not held).
struct TileListing {
    const char (*ids)[24] = nullptr;
    const char (*vers)[24] = nullptr;
    int n = 0;
};
bool lockTileList(bool poi, bool versions, TileListing& out);
void unlockTileList();

// --- Dashboard ------------------------------------------------------------
// dash_config::applyText under a lock (it parses into a static), then asks
// the UI task to repaint. False + reason as applyText.
bool applyDash(const char* text, const char** reason);

// --- What the UI task publishes for the live stream ------------------------
void readLiveView(ble_server::LiveView& out);
constexpr uint8_t LIVE_CHANGED_VIEW = 0x01;        // page / page count / screen
constexpr uint8_t LIVE_CHANGED_STOPWATCH = 0x02;   // run state, laps, reset
uint8_t takeLiveViewChanges();
// Stopwatch button from the app (ble_server::LiveStopwatchOp), run on the UI task.
void queueStopwatch(uint8_t op);

}  // namespace ble_shared
