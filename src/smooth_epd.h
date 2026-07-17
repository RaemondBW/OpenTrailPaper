#pragma once

// Experimental "smooth" e-paper drive mode (Modos-style continuous scan).
//
// Milestone 1: a self-test that proves we can drive the panel continuously
// through epdiy's own low-level LCD primitives with our own source data,
// reusing epdiy's TPS65185 power + CKV/gate timing. It pulses the whole panel
// dark <-> light for a couple of seconds, then clears and returns. No per-pixel
// state machine yet (that's M2).
//
// This is NOT the normal refresh path. It is invoked manually from the GPS
// debug screen and blocks the UI task until it finishes, so there is no
// cross-task contention over the display.

namespace smooth_epd {

// Run the M1 pipeline self-test. Blocks (~2 s), leaves the panel cleared, and
// restores epdiy to its normal state (the next UI refresh redraws). Safe to
// call from the UI task only.
void selfTest();

}  // namespace smooth_epd
