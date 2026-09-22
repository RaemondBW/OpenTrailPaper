#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// The one I2C bus (Wire) is shared by the fuel gauge (battery task), the touch
// controller + IO expander (UI task), and the RTC. Concurrent transactions from
// different tasks corrupt each other — reads come back 0x0000/0xFFFF. Every I2C
// access must be wrapped in i2cLock()/i2cUnlock(). Created in setup() before any
// task starts; the guards no-op until then.

// These also hold light sleep off for the duration, like sdLock(). The IDF I2C
// driver takes only an APB-frequency PM lock for a transaction (i2c.c), which
// stops DFS but not light sleep — so the other core could put the SoC to sleep
// with a gauge or IO-expander transfer mid-byte. A peripheral that resumes
// with a status the driver's ISR cannot clear re-asserts its level-1 interrupt
// forever, and the dispatcher serves one interrupt per entry, so that storm
// starves the tick that feeds the interrupt watchdog. Four of five
// interrupt-watchdog dumps had core 0 in that ISR
// (investigations/iwdt-light-sleep-2026-09-19.md). Mutex first, then the PM
// lock, same order as sdLock(), so a waiter does not pin the CPU awake.

#include "power_mgmt.h"

extern SemaphoreHandle_t g_i2cMutex;

inline void i2cLock()   {
    if (g_i2cMutex) xSemaphoreTake(g_i2cMutex, portMAX_DELAY);
    power_mgmt::busyAcquire();
}
inline void i2cUnlock() {
    power_mgmt::busyRelease();
    if (g_i2cMutex) xSemaphoreGive(g_i2cMutex);
}
