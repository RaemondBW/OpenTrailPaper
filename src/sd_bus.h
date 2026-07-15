#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// The SD card (SPI) is shared by the ride recorder (writes FIT 1 Hz), the UI
// task (reads map tiles on demand while rendering), the BLE server (saves
// downloaded maps/firmware), diag logging, and route storage. Concurrent SPI
// transactions from different tasks corrupt the card. Every unit of SD work
// (a file open->io->close, or a directory scan) must be wrapped in
// sdLock()/sdUnlock(). Created in setup() before any task starts; the guards
// no-op until then. Recursive so a locked helper can call another safely.

extern SemaphoreHandle_t g_sdMutex;

inline void sdLock()   { if (g_sdMutex) xSemaphoreTakeRecursive(g_sdMutex, portMAX_DELAY); }
inline void sdUnlock() { if (g_sdMutex) xSemaphoreGiveRecursive(g_sdMutex); }
