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
// Laps are stored as the clock's elapsed time at each LAP press, in a fixed
// array (no heap): lap i's split is end[i] - end[i-1]. The lap in progress is
// the time since the last mark.
//
// HOST-SAFE: no Arduino here, the caller passes `now` (millis()), so
// tools/dash_test covers it. Unsigned subtraction keeps it right across the
// 49-day millis() wrap.
constexpr int STOPWATCH_MAX_LAPS = 99;

struct Stopwatch {
    bool running = false;
    uint32_t startMs = 0;   // millis() at the last start, while running
    uint32_t accumMs = 0;   // time banked by earlier start/stop runs
    uint32_t lapEndMs[STOPWATCH_MAX_LAPS] = {};   // elapsed at each LAP
    int lapCount = 0;

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

    // Close the lap in progress. Only while running, and only until the
    // array is full — the 100th press does nothing rather than dropping lap 1.
    bool lap(uint32_t now) {
        if (!running || lapCount >= STOPWATCH_MAX_LAPS) return false;
        lapEndMs[lapCount++] = elapsedMs(now);
        return true;
    }
    bool lapsFull() const { return lapCount >= STOPWATCH_MAX_LAPS; }

    // Completed lap i's own time (0-based).
    uint32_t lapSplitMs(int i) const {
        return lapEndMs[i] - (i > 0 ? lapEndMs[i - 1] : 0);
    }
    // The lap in progress: everything since the last mark.
    uint32_t currentLapMs(uint32_t now) const {
        return elapsedMs(now) - (lapCount > 0 ? lapEndMs[lapCount - 1] : 0);
    }

    // Fastest / slowest completed lap, or -1 with fewer than two laps (one
    // lap is both, which says nothing).
    int fastestLap() const { return extremeLap(true); }
    int slowestLap() const { return extremeLap(false); }

    // Only a STOPPED clock resets — a stray tap mid-effort must not throw the
    // time away. Clears the laps too. Returns whether it did.
    bool reset() {
        if (running) return false;
        accumMs = 0;
        lapCount = 0;
        return true;
    }

private:
    int extremeLap(bool fastest) const {
        if (lapCount < 2) return -1;
        int best = 0;
        for (int i = 1; i < lapCount; ++i) {
            uint32_t a = lapSplitMs(i), b = lapSplitMs(best);
            if (fastest ? a < b : a > b) best = i;
        }
        return best;
    }
};
