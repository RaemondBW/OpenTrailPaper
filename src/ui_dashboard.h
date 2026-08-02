#pragma once

// E-paper dashboard. Landscape 960x540, epdiy highlevel API.
// Fast MODE_DU refresh each second; MODE_GC16 full clear every
// FULL_REFRESH_EVERY cycles to remove ghosting.
//
// Touch: bottom-right quadrant toggles ride recording.

namespace ui_dashboard {

// Bring up the panel, touch and input interrupts only — no SD, no radios. Call
// this EARLY in setup() so the display can show boot progress while the slow
// subsystems start; the e-paper otherwise holds the previous session's image
// for the several seconds setup() takes, which reads as a dead device.
bool beginPanel();

// Append a step to the boot screen and repaint. No-op before beginPanel() and
// after begin() hands the panel to the dashboard.
void bootStatus(const char* step, bool ok);

// Load the map and finish UI bring-up. Needs the SD card, so it runs after it.
bool begin();

// FreeRTOS task: polls touch, redraws at 1 Hz.
void task(void* arg);

}
