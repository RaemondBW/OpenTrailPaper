#pragma once

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
