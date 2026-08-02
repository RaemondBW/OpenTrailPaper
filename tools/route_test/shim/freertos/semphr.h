#pragma once
#include "FreeRTOS.h"
inline int xSemaphoreTakeRecursive(SemaphoreHandle_t, unsigned long) { return 1; }
inline int xSemaphoreGiveRecursive(SemaphoreHandle_t) { return 1; }
