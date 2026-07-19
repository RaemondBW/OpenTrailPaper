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

// Firmware-update status, for the on-device "Updating firmware" popup.
bool updateInProgress();      // true while receiving or flashing an OTA image
int  updatePercent();         // 0..100
const char* updatePhase();    // "Downloading" or "Installing"

}
