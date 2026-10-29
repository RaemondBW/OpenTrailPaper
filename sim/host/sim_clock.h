// The sim's clocks. Device time (millis/micros, every tick and delay) runs at
// `factor` × real time; the wall clock is device time plus an offset the
// firmware sets (settimeofday) or a test does, and starts unset (1970).
#pragma once
#include <stdint.h>
#include <chrono>

namespace sim_clock {
uint64_t micros64();
inline uint32_t millis() { return (uint32_t)(micros64() / 1000); }
void sleepMs(uint32_t ms);
std::chrono::nanoseconds realDuration(uint32_t ms);
void setFactor(double f);
double factor();
// Wall clock (UTC), microseconds since the epoch.
int64_t wallMicros();
void setWallMicros(int64_t us);
}  // namespace sim_clock
