#pragma once
// Host shim: sd_bus.h needs the recursive-mutex handle type only.
using SemaphoreHandle_t = void*;
#define portMAX_DELAY 0xffffffffUL
