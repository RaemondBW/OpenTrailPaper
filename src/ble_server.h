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
// Notify the phone of the device's map-layer mask ([0xE0][u16]) after an edit
// on the device's MAP LAYERS screen.
void pushMapLayersToPhone();

// True while a phone (the companion app) is connected — used to hold off
// auto-sleep during transfers.
bool isPhoneConnected();
// A central is connected but not admitted yet: encrypting with the paired
// phone's keys or, unpaired, pairing (the rider may be typing the code).
// Power management keeps the radio awake for it — a link that dies of a
// supervision timeout mid-handshake never becomes the phone.
bool linkPending();
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

// Fixed pairing. The device belongs to ONE phone: the first to pair (passkey
// shown on the panel, LE Secure Connections) is bonded, and from then on only
// that bond's authenticated link is admitted. Any other central — including
// the paired phone after it forgot the device — is disconnected before it can
// be asked to pair, and every characteristic and CCCD requires an
// authenticated link, so nothing else can read, write or subscribe meanwhile.
// Unpairing from the panel deletes the bond(s) and opens the device to the
// next phone. Details: "Fixed pairing" in ble_server.cpp.
//
// phonePaired(): true while a phone bond exists (false as soon as an unpair is
// requested, so the panel never shows a stale "Paired"). pairedPhoneCount() is
// normally 0 or 1; it can be higher on a device migrated from before fixed
// pairing, which keeps every bond it already had until the rider unpairs.
bool phonePaired();
int pairedPhoneCount();
// The paired phone's name as it reports it over GAP, ASCII-folded for the
// panel's fonts. "" when unknown (not read yet, or the phone won't say).
const char* pairedPhoneName();
// Unpair: delete the phone bond(s), drop the link, reopen pairing. Called from
// the UI task; the work runs on the server task.
void requestUnpair();
// AMS client hook: the phone's GAP Device Name, read over the admitted link.
void notePhoneName(const char* utf8);

// What the device's pages show that only the UI task knows, for the app's live
// Ride view (the phone link's LiveFrames, src/link.cpp). The UI task
// publishes it every loop pass; the link task reads it while the app
// subscribes to those topics, and sends a frame at once when the page or the
// stopwatch's state (not its running time) changes.
struct LiveView {
    unsigned char page = 0xFF;       // dash_config page on screen; 0xFF = none
    unsigned char pageCount = 0;
    unsigned char screen = 2;        // 0 dashboard page, 1 map, 2 anything else
    bool swRunning = false;
    unsigned long swAccumMs = 0;     // Stopwatch::accumMs
    unsigned long swStartMs = 0;     // Stopwatch::startMs (millis()), running
    unsigned char swLaps = 0;
    unsigned long swLastLapEndMs = 0;   // elapsed at the last LAP (0 = none)
    unsigned long swLastSplitMs = 0;    // that lap's own time (0 = none)
    unsigned short swLapSpeedDkmh = 0xFFFF;   // lap in progress, 0.1 km/h
    unsigned short swLapPowerW = 0xFFFF;
    unsigned char swLapCadenceRpm = 0xFF;
    bool swShowPower = false;
    bool swShowCadence = false;
    // The newest completed laps, newest first (row 0 = lap swLaps), as many
    // as the page's table shows (LIVE_SW_ROWS = ui_render's kSwRows). Same
    // encodings and sentinels as Stopwatch's own per-lap arrays.
    static constexpr int LIVE_SW_ROWS = 7;
    unsigned char swRows = 0;
    unsigned long swRowSplitMs[LIVE_SW_ROWS] = {};
    unsigned short swRowSpeedDkmh[LIVE_SW_ROWS] = {};   // 0xFFFF none
    unsigned short swRowPowerW[LIVE_SW_ROWS] = {};      // 0xFFFF none
    unsigned char swRowCadenceRpm[LIVE_SW_ROWS] = {};   // 0xFF none
};
void publishLiveView(const LiveView& v);

// Stopwatch buttons pressed in the app (the link's StopwatchCommand). The link
// task only queues them; the UI task takes them one at a time, oldest first,
// and runs each exactly as the page's own tap (same canReset/canLap rules).
// 0 = nothing queued.
enum LiveStopwatchOp : unsigned char {
    LIVE_SW_TOGGLE = 1,   // START <-> PAUSE
    LIVE_SW_RESET = 2,    // only while paused with time on the clock
    LIVE_SW_LAP = 3,      // only while running
};
unsigned char takeStopwatchCommand();

// Queue a media transport command (media_state.h MediaCmd) for the phone.
// Called from the UI task; the server task sends the notify.
void mediaCommand(unsigned char cmd);

// Firmware-update status, for the on-device "Updating firmware" popup.
bool updateInProgress();      // true while receiving or flashing an OTA image
int  updatePercent();         // 0..100
const char* updatePhase();    // "Downloading" or "Installing"

}
