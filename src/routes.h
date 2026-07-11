#pragma once

// GPX route loading + along-route progress. Routes live as .gpx files in
// /routes on the SD card; points are parsed into PSRAM and decimated to
// a fixed cap. Progress advances monotonically along the route as GPS
// fixes arrive.

#include <cstddef>
#include <cstdint>

namespace routes {

constexpr int MAX_NAME = 32;

bool begin();  // ensures /routes exists (needs SD mounted)

// Lists .gpx files, newest-name-first. Returns count.
int list(char names[][MAX_NAME], int maxNames);

bool load(const char* filename);  // parse /routes/<filename> from SD
// Parse a GPX already in memory (BLE upload); no SD required. Also saved
// to /routes/<name> when an SD card is present.
bool loadFromMemory(const char* name, const char* gpx, size_t len);
void clearRoute();

bool active();
const char* activeName();
int pointCount();
void point(int i, double& lat, double& lon);

// Called with each GPS fix; advances the progress index.
void updateProgress(double lat, double lon);
int progressIndex();
float remainingKm();

// --- Turn-by-turn navigation -------------------------------------------
// Maneuvers (turn cues) are uploaded alongside the route geometry. The
// device prompts to start navigation, then shows the next turn.
constexpr int MANEUVER_TEXT = 48;

void clearManeuvers();
void addManeuver(double lat, double lon, const char* instruction);
void finishManeuvers();   // all maneuvers received -> raise the nav prompt
int  maneuverCount();

// Set true once a full route+maneuver set arrives, until the user starts
// or dismisses navigation (drives the "Start navigation?" prompt).
bool navPending();
void startNav();      // enter active turn-by-turn
void dismissNav();    // keep the route drawn but stop prompting/navigating
bool navActive();

// Next turn relative to the current position (set by updateProgress).
// Returns false when there is no upcoming maneuver.
bool nextTurn(char* instruction, int textLen, float& distanceM);

}
