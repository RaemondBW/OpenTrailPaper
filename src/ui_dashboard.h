#pragma once

// E-paper dashboard. Landscape 960x540, epdiy highlevel API.
// Fast MODE_DU refresh each second; MODE_GC16 full clear every
// FULL_REFRESH_EVERY cycles to remove ghosting.
//
// Touch: bottom-right quadrant toggles ride recording.

namespace ui_dashboard {

bool begin();

// FreeRTOS task: polls touch, redraws at 1 Hz.
void task(void* arg);

}
