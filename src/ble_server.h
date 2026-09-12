#pragma once

// BLE GATT server for the iOS companion app. Runs alongside the sensor
// central (both roles share the one NimBLEDevice). Exposes:
//   - Settings  (read/write): FTP watts + timezone minutes
//   - Status    (notify):     battery, GPS fix, sats, speed
//   - Route     (write):      chunked GPX upload -> active route
//
// Service/characteristic UUIDs are mirrored in the iOS app (BLEManager).

namespace ble_server {

// Call after NimBLEDevice::init() (done by ble_sensors::begin()).
void begin();

// FreeRTOS task: pushes a status notification once a second.
void task(void* arg);

// Mirror a device-side settings edit (FTP/tz/units/backlight) to the phone.
void pushSettingsToPhone();

// True while a phone (the companion app) is connected — used to hold off
// auto-sleep during transfers.
bool isPhoneConnected();
// Monotonic within a boot; reconnects get a new ID even if a sample missed
// the disconnected period. Used to verify sleep on an uninterrupted link.
unsigned long phoneConnectionId();

// Actual negotiated interval >= 100 ms; used by the legacy sleep gate.
// The MAIN_XTAL build can maintain the phone at either interval.
bool linkRelaxed();

// Queue diagnostics or a transient fast request (returns to idle policy after
// eight seconds without bulk traffic). Mutable policy stays on the BLE task.
void reportInterval();
void requestFastInterval();

// Per-boot connected-sleep experiment. Default off on legacy clock builds,
// on with MAIN_XTAL; three supervision timeouts disable it for this boot.
bool relaxedSleepAllowed();
void setRelaxedSleepExperiment(bool on);

// True once (and cleared) when the phone has written a new dashboard layout, so
// the UI task can repaint immediately instead of waiting for the next 1 Hz tick.
bool takeDashChanged();

// The 6-digit BLE pairing code to show on the panel; 0 when no pairing is in
// progress. Set while the phone's pairing dialog is up, self-expiring.
unsigned int pairingCode();

// Queue a media transport command (media_state.h MediaCmd) for the phone.
// Called from the UI task; the server task sends the notify.
void mediaCommand(unsigned char cmd);

// Firmware-update status, for the on-device "Updating firmware" popup.
bool updateInProgress();      // true while receiving or flashing an OTA image
int  updatePercent();         // 0..100
const char* updatePhase();    // "Downloading" or "Installing"

}
