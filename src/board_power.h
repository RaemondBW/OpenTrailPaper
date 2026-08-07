#pragma once

#include <stddef.h>

// Board-level power helpers implemented in main.cpp (which owns the IO
// expander instance).

// Gates the GPS + LoRa 3V3 rail (XL9555 IO0).
void board_radio_power(bool on);

// Side button on the IO expander (PC12), pressed = true.
bool board_side_button_pressed();

// Instantaneous battery voltage (mV) and current (mA, signed: negative =
// discharging) from the fuel gauge. Returns false if the gauge is absent.
// Used by the `power` console command to measure the GPS rail's draw.
bool board_read_power(uint16_t& mv, int16_t& ma);

// Full fuel-gauge snapshot as one printable line: state of charge, voltage,
// current, capacities, health, temperature and the raw status words. Same text
// the battery task writes to the diagnostic log, on demand — for debugging a
// missing / stuck battery percentage without waiting for the 2 min sample.
// Writes into `out`; returns false (with a reason in `out`) if init() failed.
bool board_gauge_report(char* out, size_t n);
