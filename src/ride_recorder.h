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

// Stats of the ride in progress (or the one just stopped) for the
// summary screen.
RideSummary summary();

// SD card info for the menu screen.
bool sdMounted();
int rideCount();
uint32_t sdFreeMB();

// FreeRTOS task: ticks at RECORD_INTERVAL_MS while recording.
void task(void* arg);

}
