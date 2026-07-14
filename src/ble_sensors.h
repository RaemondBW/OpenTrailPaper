#pragma once

#include <cstdint>

// BLE central for standard cycling sensors:
//   Heart Rate           0x180D / measurement 0x2A37
//   Cycling Power        0x1818 / measurement 0x2A63 (also yields cadence
//                        when the meter reports crank revolutions)
//   Speed & Cadence      0x1816 / measurement 0x2A5B
//
// Pairing: addresses saved via settings are preferred; with nothing
// saved, the first device advertising each service is used. The Sensors
// screen lists scan candidates and pairs/forgets explicitly.

namespace ble_sensors {

enum Kind { KIND_HR = 0, KIND_POWER = 1, KIND_CSC = 2, KIND_COUNT = 3 };

struct Candidate {
    char name[32];      // advertised name, or "Manufacturer Model" once connected
    char addr[18];
    uint8_t kindsMask;  // bit per Kind
    int8_t rssi;
    bool connected;     // currently connected to us
    bool paired;        // saved in settings
};

void begin();
void task(void* arg);

// Sensors screen support
void setScanAlways(bool on);            // keep scanning while the UI is open
void noteActivity();                    // user interacted — scan for a while
int getCandidates(Candidate* out, int maxOut);
void pairCandidate(const char* addr);   // saves for every kind it advertises
void forget(const char* addr);          // clear one paired address
void forgetAll();

}
