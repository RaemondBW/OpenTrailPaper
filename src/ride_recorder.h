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

// SD card info for the menu screen.
bool sdMounted();
int rideCount();
uint32_t sdFreeMB();

// FreeRTOS task: ticks at RECORD_INTERVAL_MS while recording.
void task(void* arg);

}
