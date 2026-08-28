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

// Apple Find My beacon (OpenHaystack): re-read the key/enable settings, and
// whether a valid key is currently being broadcast.
void findMyRefresh();
bool findMyBeaconing();

// True while a phone (the companion app) is connected — used to hold off
// auto-sleep during transfers.
bool isPhoneConnected();

// True while the connection is MEASURED to be running at a long interval
// (>= 100 ms — the relaxed state the conn-param governor asks for while
// riding). power_mgmt uses this to allow CPU light sleep with the phone
// attached: the supervision-timeout failures that forced the always-hold
// happened at a 30 ms interval, where the sleep clock's drift budget is 10x
// tighter.
bool linkRelaxed();

// The light-sleep-with-phone experiment gate. DEFAULT OFF since 2026-08-19:
// the relaxed interval did not save the link (3 supervision timeouts in 41 s),
// so power_mgmt holds sleep off whenever the phone is connected. Re-armable
// from the console (`sleepexp on`) for a future run after a controller-clock
// fix; three timeouts while armed turn it back off for the boot.
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
