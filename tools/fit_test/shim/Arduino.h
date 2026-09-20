// Host shim: the sliver of Arduino.h that fit_writer.cpp needs.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

// repair() yields to the watchdog between chunks on the device.
inline void delay(unsigned long) {}
