#pragma once

// OPTIONAL sensors on the Qwiic connector, on the same I2C bus as everything
// else (see i2c_bus.h — every access here is guarded):
//
//   BME280      0x76/0x77   pressure -> barometric altitude
//   LSM303AGR   0x19 accel, 0x1E mag   movement detection + compass
//
// Optional means OPTIONAL: begin() probes for each chip independently and the
// device behaves exactly as it did without them when they are absent, one is
// absent, or one stops answering. Nothing here is required for a ride.

#include <cstddef>
#include <cstdint>

namespace aux_sensors {

// Probe the bus. Safe to call with nothing attached; logs what it found.
// Returns true if at least one chip answered.
bool begin();

// 5 Hz reader task. Not created when begin() found nothing.
void task(void* arg);

bool haveBaro();
bool haveCompass();

// Sea-level reference the altimeter is currently working against, in pascals,
// and whether it has been calibrated from a known elevation rather than left at
// the ISA default. For the diagnostics screen.
float seaLevelPa();
bool  seaLevelCalibrated();

// One line of state for the log / GPS-debug page.
void report(char* out, size_t n);

}  // namespace aux_sensors
