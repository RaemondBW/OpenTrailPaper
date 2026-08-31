#pragma once

#include "ride_state.h"

// Owns the ride lifecycle: start/stop, 1 Hz FIT records, distance and
// stat accumulation (moving time, averages, normalized power, climbing,
// grade), periodic SD flush.

namespace ride_recorder {

bool begin();  // mounts SD, ensures RIDE_DIR exists

void startRide();

// Finalizes the FIT file; save=false deletes it (summary screen DISCARD).
void stopRide(bool save);
bool isRecording();

// True once an auto-pause has lasted long enough (2 min) that the rider is
// clearly parked, not waiting out a light. Gates the power policy for the
// stop: ble_sensors stops hunting and power_mgmt drops the sensor-link hold
// so the CPU can sleep — the power meter sleeps itself at a coffee stop and
// the links are going away regardless. The instant movement resumes the ride
// unpauses, this goes false, and the hunt + no-sleep hold return in the same
// tick, so reconnection runs at full radio quality.
bool longAutoPaused();

// Rider tapped the AUTO-PAUSED banner: resume the timer now. Movement gets a
// 30 s head start before stillness counts toward re-pausing — the tap means
// "I'm leaving", and clipping in takes a moment. No-op unless auto-paused.
void manualResume();

// Basename (no directory) of the FIT file currently being recorded, or ""
// when idle. Lets the history screen skip the still-open, unfinalized file.
const char* currentRideFile();

// Stats of the ride in progress (or the one just stopped) for the
// summary screen.
RideSummary summary();

// Re-mount the card: SD.end() then SD.begin(), under the SD lock, updating the
// mount state. Refuses while a ride is recording — pulling the filesystem out
// from under an open FIT file is the interrupted transaction that wedges a card.
// `why` is logged. Returns the new mount state.
bool remount(const char* why);

// Call ~1 Hz from loop(). While the card is not mounted, retries it on a slow
// cadence so a card that comes back — reseated, or recovered after a bad
// transaction — works again without a reboot. Mounting used to happen exactly
// once at boot, so any drop was permanent for the session.
void retryMountIfNeeded();

// What boot found when a reset or power cut interrupted a recording ride.
// recoverRides() auto-repairs older leftovers but holds the NEWEST one for a
// decision; pendingRecovery() then hands its replayed stats to the UI, which
// shows the interrupted-ride sheet and answers with resolveRecovery().
enum RecoveryAction {
    RECOVERY_CONTINUE,   // reopen the file and keep recording into it
    RECOVERY_SAVE,       // finalize it as a complete ride (repair)
    RECOVERY_DISCARD,    // delete it
};
bool pendingRecovery(RideSummary* out);
void resolveRecovery(RecoveryAction action);

// Rebuild rides that a reset or power cut left half-written. MUST NOT be called
// from setup(): the work is proportional to what is on the card and once blew
// the interrupt watchdog, and a reset mid-recovery tears one more file, so the
// next boot finds more work than the last — a boot loop with no escape short of
// removing the card. The UI task calls this once it is running, before the SD
// is offered to a USB host.
void recoverRides();

// True once after a card mounted later than boot did. Boot skips routes, the
// map index and USB mass storage when the mount fails, and nothing used to
// revisit that — so a card picked up by the retry above was mounted but
// entirely unused (no maps, no routes) until the next reboot. The UI task
// consumes this and re-runs that bring-up.
bool consumeLateMount();

// SD card info for the menu screen.
bool sdMounted();
int rideCount();
uint32_t sdFreeMB();

// FreeRTOS task: ticks at RECORD_INTERVAL_MS while recording.
void task(void* arg);

}
