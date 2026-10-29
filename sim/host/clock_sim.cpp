#include "sim_clock.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <sys/time.h>
#include <time.h>

namespace {
using Clock = std::chrono::steady_clock;
std::mutex m;
Clock::time_point realBase = Clock::now();
uint64_t simBaseUs = 0;
double fac = 1.0;
std::atomic<int64_t> wallOffsetUs{0};   // wall = sim + offset; 0 = 1970 at boot
}

namespace sim_clock {

uint64_t micros64() {
    std::lock_guard<std::mutex> lk(m);
    auto real = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - realBase).count();
    return simBaseUs + (uint64_t)(real * fac);
}

void setFactor(double f) {
    if (f <= 0) return;
    uint64_t now = micros64();
    std::lock_guard<std::mutex> lk(m);
    simBaseUs = now;
    realBase = Clock::now();
    fac = f;
}

double factor() { std::lock_guard<std::mutex> lk(m); return fac; }

std::chrono::nanoseconds realDuration(uint32_t ms) {
    return std::chrono::nanoseconds((int64_t)(ms * 1e6 / factor()));
}

// A long sleep is taken in slices against the device clock, so a `speed`
// change lands at once: a module ticking once a device-second (the GPS) must
// not sit out a whole real second at the old rate after the clock is sped up
// — at 10x that is ten device-seconds of silence, and the firmware reads it
// as a lost fix.
void sleepMs(uint32_t ms) {
    if (ms < 50) { std::this_thread::sleep_for(realDuration(ms)); return; }
    const uint64_t until = micros64() + (uint64_t)ms * 1000;
    for (;;) {
        const uint64_t now = micros64();
        if (now >= until) return;
        const uint32_t left = (uint32_t)((until - now + 999) / 1000);
        std::this_thread::sleep_for(std::min<std::chrono::nanoseconds>(realDuration(left),
                                                                       std::chrono::milliseconds(20)));
    }
}

int64_t wallMicros() { return (int64_t)micros64() + wallOffsetUs.load(); }
void setWallMicros(int64_t us) { wallOffsetUs = us - (int64_t)micros64(); }

}  // namespace sim_clock

// The firmware's time(), gettimeofday() and settimeofday() resolve to these
// (the executable's definitions win over libSystem's for its own objects).
extern "C" {
time_t time(time_t* out) {
    time_t t = (time_t)(sim_clock::wallMicros() / 1000000);
    if (out) *out = t;
    return t;
}
int gettimeofday(struct timeval* tv, void*) {
    int64_t us = sim_clock::wallMicros();
    if (tv) { tv->tv_sec = (time_t)(us / 1000000); tv->tv_usec = (suseconds_t)(us % 1000000); }
    return 0;
}
int settimeofday(const struct timeval* tv, const struct timezone*) {
    if (tv) sim_clock::setWallMicros((int64_t)tv->tv_sec * 1000000 + tv->tv_usec);
    return 0;
}
}
