#pragma once

#include <stdint.h>

// The STOPWATCH page's clock (`page stopwatch` in the dashboard config).
//
// Deliberately separate from the ride timer: it is the rider's own clock for a
// climb, an interval or a coffee stop, and starting, stopping or resetting it
// never touches the ride. Time is measured from millis() stamps rather than
// counted per frame, so it keeps running exactly while the rider is on another
// page, in a menu, or the SoC is in light sleep. A reboot or power-off clears
// it, like any stopwatch with its batteries out.
//
// HOST-SAFE: no Arduino here, the caller passes `now` (millis()), so
// tools/dash_test covers it. Unsigned subtraction keeps it right across the
// 49-day millis() wrap.
struct Stopwatch {
    bool running = false;
    uint32_t startMs = 0;   // millis() at the last start, while running
    uint32_t accumMs = 0;   // time banked by earlier start/stop runs

    uint32_t elapsedMs(uint32_t now) const {
        return accumMs + (running ? now - startMs : 0);
    }

    // Untouched since the last reset: nothing to reset, START reads "START".
    bool zero() const { return !running && accumMs == 0; }

    // START <-> STOP. Stopping banks the run; starting again continues from it.
    void toggle(uint32_t now) {
        if (running) {
            accumMs += now - startMs;
            running = false;
        } else {
            startMs = now;
            running = true;
        }
    }

    // Only a STOPPED clock resets — a stray tap mid-effort must not throw the
    // time away. Returns whether it did.
    bool reset() {
        if (running) return false;
        accumMs = 0;
        return true;
    }
};
