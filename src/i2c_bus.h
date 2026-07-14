#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// The one I2C bus (Wire) is shared by the fuel gauge (battery task), the touch
// controller + IO expander (UI task), and the RTC. Concurrent transactions from
// different tasks corrupt each other — reads come back 0x0000/0xFFFF. Every I2C
// access must be wrapped in i2cLock()/i2cUnlock(). Created in setup() before any
// task starts; the guards no-op until then.

extern SemaphoreHandle_t g_i2cMutex;

inline void i2cLock()   { if (g_i2cMutex) xSemaphoreTake(g_i2cMutex, portMAX_DELAY); }
inline void i2cUnlock() { if (g_i2cMutex) xSemaphoreGive(g_i2cMutex); }
