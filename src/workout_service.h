#pragma once

#include <stdint.h>
#include <stddef.h>
#include "workout.h"

// Firmware half of the workout feature: files on the card, the running clock,
// and the console's verbs. The parsing/time math lives in workout.* (host-safe
// so the preview tool can render the page); this half owns the SD access and
// the one loaded workout.
//
// Files live in /workouts on the card — .erg or .mrc, the export every
// training platform offers. The rider loads one (console `workout load`, or
// the app later), presses start, and the workout clock runs on wall time from
// that moment. v1 keeps the clock independent of the ride recorder: a trainer
// session may not even be recording, and a workout mid-ride shouldn't pause
// with a stoplight auto-pause while the intervals march on.
namespace workout_service {

// List /workouts into `out` as one name per line (for the console / the app).
// Returns the number of files seen.
int list(char* out, size_t cap);

// Load /workouts/<name> (extension included) and make it the active workout.
// Stops any running session. On failure the previous workout is dropped too —
// half a workout is worse than none. `reason` gets a static string for logs.
bool load(const char* name, const char** reason);

// Start / stop the workout clock. start() from DONE (or mid-way) restarts
// from zero — the natural meaning of pressing start again. pause()/resume()
// hold the clock without losing the position; toggle() is the play button
// (start -> pause -> resume as appropriate).
void start();
void stop();
// stop() and forget the loaded workout entirely — the page goes back to
// "no workout loaded". The app's Stop button means this.
void unload();
void pause();
void resume();
void toggle();
// Jump the clock to the current segment's end: the escape hatch for "the
// warmup is over, I'm warm".
void skip();
// Restart the current interval — or, within its first 3 s, the one before
// (the media-player convention thumbs already know).
void prevInterval();
// Jump to the start of the interval containing `frac` (0..1) of the total
// duration — the profile strip's tap-to-seek. Returns false if not loaded.
bool jumpToFraction(float frac);
// Start block `idx` (clamped) from its beginning, running — the ALL BLOCKS
// list's tap-to-start, and the prev/next block strips.
void jumpToSeg(int idx);

// 1 Hz housekeeping from loop(): enforces the pause-after-every-block
// setting (settings::workoutPauseEachBlock) at boundaries.
void tick();

bool loaded();
bool running();

// Fill `v` for the renderer, from the loaded workout and the clock.
void view(WorkoutView& v);

}  // namespace workout_service
