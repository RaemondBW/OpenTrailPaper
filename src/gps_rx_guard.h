#pragma once
#include <stdint.h>
namespace gps_rx_guard {
void begin();
void tick(bool receivedBytes);
// Called only by the IRAM light-sleep wrapper, with interrupts potentially off.
bool beforeSleep();
void afterSleep();
void report();
}
