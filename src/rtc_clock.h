#pragma once

// PCF8563 real-time clock on the shared I2C bus (addr 0x51), backed by the J12
// coin cell. Unlike the ESP32's internal clock — which only survives deep sleep
// — this keeps accurate UTC across a full power-off. We restore the system clock
// from it at boot so GPS time-aiding (a warm start) works even on a cold boot,
// and write GPS time back to it so the coin cell stays accurate. All access goes
// through the i2cLock()/i2cUnlock() guards.

#include <ctime>

namespace rtc_clock {

// Probe for the chip. Returns true if it ACKs on the bus.
bool begin();

// Read UTC into `out`. Returns false if the chip is absent, its clock-integrity
// (VL) flag is set (battery was removed / never set), or the value is implausible.
bool read(time_t& out);

// Set the chip's time from a unix UTC timestamp (clears the VL flag).
void write(time_t utc);

}  // namespace rtc_clock
