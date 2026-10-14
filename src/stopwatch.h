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
// Each lap also gets an average speed and average power, integrated from
// sample() calls the UI task makes every loop pass (a few per second, on
// whatever page is showing). The values passed are HELD until the next
// sample — a zero-order hold, which is how the sensors behave — so a lap or a
// stop between two samples is still credited up to its exact instant. Only
// running time counts:
//   - speed: distance (the dashboard's speed, integrated) over the running
//     time that HAD a speed source. No source for the whole lap = no value.
//   - power: time-weighted mean of the readings over the running time a power
//     meter was reporting. Zeros count (coasting is part of the lap, as on most
//     head units); a meter gone silent does not. None at all = no value.
//   - cadence: time-weighted mean of the NONZERO readings — coasting at 0 rpm
//     is not pedalling, so it doesn't drag the average down (the usual head
//     unit default). No source, or only zeros = no value.
//
// Which metric columns the page shows follows from the same data:
// showPower()/showCadence() — a source reporting now, or any data in the
// lap in progress or a completed lap since the last reset.
//
// HOST-SAFE: no Arduino here, the caller passes `now` (millis()), so
// tools/dash_test covers it. Unsigned subtraction keeps it right across the
// 49-day millis() wrap.
constexpr int STOPWATCH_MAX_LAPS = 99;
constexpr uint16_t STOPWATCH_NO_VALUE = 0xFFFF;
constexpr uint8_t STOPWATCH_NO_CADENCE = 0xFF;

struct Stopwatch {
    bool running = false;
    uint32_t startMs = 0;   // millis() at the last start, while running
    uint32_t accumMs = 0;   // time banked by earlier start/pause runs
    uint32_t lapEndMs[STOPWATCH_MAX_LAPS] = {};   // elapsed at each LAP
    // Per completed lap: avg speed in 0.1 km/h, avg power in W
    // (STOPWATCH_NO_VALUE = none), avg cadence in rpm (STOPWATCH_NO_CADENCE =
    // none). 5 bytes a lap.
    uint16_t lapSpeedDkmh[STOPWATCH_MAX_LAPS] = {};
    uint16_t lapPowerW[STOPWATCH_MAX_LAPS] = {};
    uint8_t lapCadenceRpm[STOPWATCH_MAX_LAPS] = {};
    int lapCount = 0;
    // A completed lap (since the last reset) had a meter / cadence source
    // reporting — keeps its column on screen after the sensor goes quiet.
    bool lapsHadPower = false;
    bool lapsHadCadence = false;

    // --- Lap in progress: integrals since the last mark (running time only).
    double curDistM = 0;       // speed x time
    uint32_t curSpeedMs = 0;   // running time with a speed source
    double curEnergyWms = 0;   // power x time
    uint32_t curPowerMs = 0;   // running time with a reporting meter
    double curCrankRevMs = 0;  // cadence x time, nonzero readings only
    uint32_t curPedalMs = 0;   // running time pedalling (cadence > 0)
    uint32_t curCadenceMs = 0; // running time with a cadence source, zeros too

    // The held sample.
    uint32_t sampleMs = 0;
    bool haveSample = false;
    float heldSpeedKmh = 0;
    bool heldSpeedValid = false;
    uint16_t heldPowerW = 0;
    bool heldPowerValid = false;
    uint8_t heldCadenceRpm = 0;
    bool heldCadenceValid = false;

    uint32_t elapsedMs(uint32_t now) const {
        return accumMs + (running ? now - startMs : 0);
    }

    // Untouched since the last reset: nothing to reset, START reads "START".
    bool zero() const { return !running && accumMs == 0; }

    // A new reading. Credits the PREVIOUS one over the time since it.
    void sample(uint32_t now, float speedKmh, bool speedValid, uint16_t powerW,
                bool powerValid, uint8_t cadenceRpm = 0,
                bool cadenceValid = false) {
        advance(now);
        heldSpeedKmh = speedKmh < 0 ? 0 : speedKmh;
        heldSpeedValid = speedValid;
        heldPowerW = powerW;
        heldPowerValid = powerValid;
        heldCadenceRpm = cadenceRpm;
        heldCadenceValid = cadenceValid;
        haveSample = true;
    }

    // Which optional metrics the page shows: a source reporting right now,
    // or data in the lap in progress or any completed lap. Pure function of
    // state that changes on sensor events and LAP/RESET, not per second.
    bool showPower() const {
        return heldPowerValid || curPowerMs > 0 || lapsHadPower;
    }
    bool showCadence() const {
        return heldCadenceValid || curCadenceMs > 0 || lapsHadCadence;
    }

    // START <-> PAUSE. Pausing banks the run; starting again continues from it.
    void toggle(uint32_t now) {
        advance(now);   // credit the held sample up to the stop
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
        advance(now);
        lapEndMs[lapCount] = elapsedMs(now);
        float v = currentAvgSpeedKmh();
        lapSpeedDkmh[lapCount] = v < 0 ? STOPWATCH_NO_VALUE : clampU16(v * 10.0f);
        int p = currentAvgPowerW();
        lapPowerW[lapCount] = p < 0 ? STOPWATCH_NO_VALUE : clampU16((float)p);
        int c = currentAvgCadenceRpm();
        lapCadenceRpm[lapCount] =
            c < 0 ? STOPWATCH_NO_CADENCE : (uint8_t)(c > 254 ? 254 : c);
        if (curPowerMs > 0) lapsHadPower = true;
        if (curCadenceMs > 0) lapsHadCadence = true;
        ++lapCount;
        clearCurrent();
        return true;
    }
    bool lapsFull() const { return lapCount >= STOPWATCH_MAX_LAPS; }

    // Button enablement, shared by the renderer (drawn disabled) and the tap
    // handler (does nothing), so the two can't disagree.
    //   START/PAUSE: always live.
    //   RESET: only while paused with something to clear — never mid-effort.
    //   LAP: only while running, and under the 99-lap cap.
    bool canReset() const { return !running && !zero(); }
    bool canLap() const { return running && !lapsFull(); }

    // Completed lap i's own time (0-based).
    uint32_t lapSplitMs(int i) const {
        return lapEndMs[i] - (i > 0 ? lapEndMs[i - 1] : 0);
    }
    // The lap in progress: everything since the last mark.
    uint32_t currentLapMs(uint32_t now) const {
        return elapsedMs(now) - (lapCount > 0 ? lapEndMs[lapCount - 1] : 0);
    }

    // Lap in progress, as of the last sample; -1 = no source yet.
    float currentAvgSpeedKmh() const {
        return curSpeedMs ? (float)(curDistM * 3600.0 / curSpeedMs) : -1.0f;
    }
    int currentAvgPowerW() const {
        return curPowerMs ? (int)(curEnergyWms / curPowerMs + 0.5) : -1;
    }
    int currentAvgCadenceRpm() const {
        return curPedalMs ? (int)(curCrankRevMs / curPedalMs + 0.5) : -1;
    }
    // Completed lap i; -1 = no source during it.
    float lapAvgSpeedKmh(int i) const {
        return lapSpeedDkmh[i] == STOPWATCH_NO_VALUE ? -1.0f
                                                     : lapSpeedDkmh[i] / 10.0f;
    }
    int lapAvgPowerW(int i) const {
        return lapPowerW[i] == STOPWATCH_NO_VALUE ? -1 : lapPowerW[i];
    }
    int lapAvgCadenceRpm(int i) const {
        return lapCadenceRpm[i] == STOPWATCH_NO_CADENCE ? -1 : lapCadenceRpm[i];
    }

    // Only a PAUSED clock resets — a stray tap mid-effort must not throw the
    // time away. Clears the laps and their averages too. Returns whether it did.
    bool reset() {
        if (running) return false;
        accumMs = 0;
        lapCount = 0;
        lapsHadPower = lapsHadCadence = false;
        clearCurrent();
        return true;
    }

private:
    // Credit the held sample over [sampleMs, now] if the clock is running.
    void advance(uint32_t now) {
        if (running && haveSample) {
            const uint32_t dt = now - sampleMs;
            if (heldSpeedValid) {
                curDistM += heldSpeedKmh / 3.6 * dt / 1000.0;
                curSpeedMs += dt;
            }
            if (heldPowerValid) {
                curEnergyWms += (double)heldPowerW * dt;
                curPowerMs += dt;
            }
            if (heldCadenceValid) {
                curCadenceMs += dt;
                if (heldCadenceRpm > 0) {
                    curCrankRevMs += (double)heldCadenceRpm * dt;
                    curPedalMs += dt;
                }
            }
        }
        sampleMs = now;
    }

    void clearCurrent() {
        curDistM = 0;
        curSpeedMs = 0;
        curEnergyWms = 0;
        curPowerMs = 0;
        curCrankRevMs = 0;
        curPedalMs = 0;
        curCadenceMs = 0;
    }

    static uint16_t clampU16(float v) {
        if (v < 0) return 0;
        if (v > 65534.0f) return 65534;
        return (uint16_t)(v + 0.5f);
    }
};
