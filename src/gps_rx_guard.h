#pragma once
#include <stdint.h>
namespace gps_rx_guard {
void begin();
void tick(bool receivedBytes);
// Called only by the IRAM light-sleep wrapper, with interrupts potentially off.
bool beforeSleep();
void afterSleep();
void report();
uint32_t waitMs();  // task context: quiet-deadline check or idle housekeeping
}
