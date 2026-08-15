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

// NOT only the SD card: the SX1262 LoRa module hangs off the same MISO/MOSI/SCK
// with its own chip select, so lora_radio.cpp takes this lock for every radio
// transfer too. That is also why it never holds it across a transmission — a
// LongFast packet is seconds of air time, and the recorder needs the bus at 1 Hz.

// These also hold light sleep off for the duration. The SD path runs through
// Arduino's SPIClass, which — unlike ESP-IDF's spi_master — takes no PM lock, so
// a light sleep landing mid-command gates the SPI clock and wedges the card
// until it is physically power-cycled. Since every SD access is already wrapped
// here, this is the one place that needs to know. No-op without PM.
// See power_mgmt::busyAcquire().

#include "power_mgmt.h"

extern SemaphoreHandle_t g_sdMutex;

inline void sdLock()   {
    power_mgmt::busyAcquire();
    if (g_sdMutex) xSemaphoreTakeRecursive(g_sdMutex, portMAX_DELAY);
}
inline void sdUnlock() {
    if (g_sdMutex) xSemaphoreGiveRecursive(g_sdMutex);
    power_mgmt::busyRelease();
}
