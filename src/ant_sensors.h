#pragma once

#include <cstddef>
#include <cstdint>

// ANT+ sensors on the ESP32's own radio (esp32-ant+, coexist mode).
//
// The S3 has one 2.4 GHz radio and NimBLE owns it. The ANT stack does not take
// it away: with cfg.coexist it hooks the controller's SCAN path, so every
// window of a running BLE passive scan is retuned to 2457 MHz / the ANT sync
// word and the ANT+ pages are lifted out. BLE CONNECTIONS (phone, paired BLE
// sensors) keep running between the windows; BLE SCANNING does not — the
// windows are ANT's while a receive is open. Two things follow:
//
//   1. ANT+ only receives while a perpetual passive scan is running. The scan
//      arbiter lives in ble_sensors::task: it stops ANT for the duration of a
//      BLE hunt/connect (which needs the scan windows itself) and brings it
//      back afterwards. Nothing here touches NimBLEScan.
//   2. While ANT is up the radio is never quiet, so power_mgmt must treat it
//      like a BLE sensor link (ble_sensors::anyConnected/radioBusy fold it in).
//
// Pairing follows the BLE policy: nothing is ever adopted on its own. A kind
// only opens a channel once a device of that type is REMEMBERED (the library's
// known-device table, NVS namespace "ant"), and remembering one is an explicit
// action — pair() runs a proximity-limited wildcard search for that kind and
// keeps the first master heard close by. forget() drops it.
//
// Readings are published into the same RideState fields the BLE path feeds,
// through ble_sensors' feed helpers, so the dashboard, recorder and FIT writer
// do not know which radio delivered them.

namespace ant_sensors {

// Kinds are ble_sensors::Kind (HR / Power / Cadence): one ANT channel each.
constexpr int KIND_COUNT = 3;

// Create the feed task and queue. Radio untouched — call after
// ble_sensors::begin() (NimBLE must be initialised before the node can start).
void begin();

// --- arbiter (ble_sensors::task only) --------------------------------------
// True when ANT has a reason to hold the radio: a kind is paired, or a pair
// search was requested.
bool wanted();
// A pair search is pending or running — the user is waiting on the radio.
bool pairPending();
// Bench: keep ANT wanted regardless of ride state (console `ant hold on`).
void setHold(bool on);
bool hold();
// The node is up, i.e. the BLE scan windows are ANT's right now.
bool radioActive();
// Bring the node up (a passive perpetual scan MUST already be running) and open
// a channel per paired kind / pending pair; or stop it and release the windows.
void setRadio(bool on);

// --- state ------------------------------------------------------------------
bool tracking(int kind);      // pages arriving from the paired device
bool anyTracking();

struct Link {
    char     kind[12];        // "HR" / "Power" / "Cadence"
    uint16_t deviceNum;       // remembered device, 0 = not paired
    bool     paired;
    bool     tracking;
    bool     searching;       // channel open, no device locked yet
    bool     pairing;         // a fresh pair search is in progress
    int8_t   rssi;            // last page's, dBm
    uint32_t pages;           // received this boot
    uint32_t lastPageMs;      // 0 = none yet
};
void links(Link* out);        // KIND_COUNT entries in Kind order

// --- actions (UI / console / phone) -----------------------------------------
// Search for a nearby master of that kind (proximity-gated) and remember it,
// replacing any remembered one. Times out after ANT_PAIR_TIMEOUT_S.
bool pair(int kind);
bool forget(int kind);
void forgetAll();

// Name for lists: "ANT+ HR 12345". Empty when not paired.
void displayName(int kind, char* out, size_t n);

// --- scanning ---------------------------------------------------------------
// A wildcard channel that is never allowed to acquire: every CRC-valid frame
// it hears is tallied by device type and number instead. Feeds the shared
// candidate list (ble_sensors::getCandidates), so the Sensors screen and the
// phone app list ANT+ devices next to BLE ones and pair by device number.
struct Candidate {
    uint8_t  devType;     // ANT+ device type (120 HR, 11 power, 121/122 cadence)
    uint16_t devNum;
    uint8_t  trans;
    int8_t   rssi;        // strongest seen
    uint32_t lastMs;
    uint32_t frames;
};
void scanSet(bool on);            // explicit (console / app): scan until told to stop
void scanWhileListOpen(bool on);  // arbiter: a sensors list is open on screen or phone
bool scanning();                  // either
int  candidates(Candidate* out, int maxOut);
// Remember a device by id (no search) and open its channel if the node is up.
bool pairDevice(uint8_t devType, uint16_t devNum, uint8_t trans);
int  kindForType(uint8_t devType);          // ble_sensors::Kind, -1 = unsupported
const char* typeName(uint8_t devType);      // "HR" / "Power" / "Cadence" / "type N"

// One-screen status for the console.
void printReport();
// Print the next n frames the radio decodes, matched or not (bring-up aid:
// what is actually on the air at 2457 MHz).
void tap(int n);
void types(bool start);   // start = reset + begin tallying by device type; false = print
void printBootTrace();    // the library's radio trace from before the last reset
// Hex-dump BLE controller exchange memory (bring-up aid for the coexist hook:
// which control structure is the scan's).
void dumpEm(unsigned off, unsigned len);
// Coexist bring-up knobs (see ant_espphy_t): force the scan CS format byte
// (0 = keep), or override one CS byte at every reschedule (off 0xFF = clear).
void setCsFormat(uint8_t fmt);
void setProximity(int8_t dbm);        // pair-search RSSI gate, 0 = any
void setCoexistChannel(uint8_t ch);   // 0 = library default (39)
void setKeepCrc(bool keep);           // experiment: leave BLE CRC checking on
void setPatchMask(uint8_t m);         // experiment: which CS patches to apply
void setMhz(uint16_t mhz);            // experiment: window frequency, 0 = ANT+ 2457
void setSyncOverride(const char* hex8);   // experiment: sync word to program; nullptr = off
void setCsOverride(uint8_t off, uint8_t val);

}  // namespace ant_sensors
