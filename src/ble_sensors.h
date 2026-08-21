#pragma once

#include <cstdint>

// BLE central for standard cycling sensors:
//   Heart Rate           0x180D / measurement 0x2A37
//   Cycling Power        0x1818 / measurement 0x2A63 (also yields cadence
//                        when the meter reports crank revolutions)
//   Speed & Cadence      0x1816 / measurement 0x2A5B
//
// Pairing: we connect ONLY to the address saved for a kind. With nothing
// saved for a kind, that kind never connects — no device is ever adopted
// automatically. The Sensors screen (and the phone) list scan candidates and
// pair/forget explicitly. Note that this is a saved MAC, not a BLE bond:
// these profiles are unauthenticated, so the address is the only thing
// keeping us off someone else's strap.

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

// One kind's pairing/link state, for the `sensors` console command. The UI
// shows a sensor by name once it knows one, so the saved MAC — the only thing
// that actually decides what we connect to — is otherwise invisible.
struct Link {
    char kind[12];         // "HR" / "Power" / "Cadence"
    char pairedAddr[18];   // saved in NVS; "" when nothing is paired
    char liveAddr[18];     // address of the live link; "" when not connected
    char name[32];         // best identity known: DIS make > NVS > advertised
    bool connected;
};

// The last Heart Rate Measurement (0x2A37) payload we received. Flags bit 0 is
// the 8/16-bit value format, bit 1 is "contact detected", bit 2 is "contact
// detection supported" — the pair that says whether an unworn strap's number
// means anything.
struct HrPacket {
    uint8_t  flags;
    uint8_t  len;
    uint16_t bpm;
    uint32_t atMs;   // millis() on arrival; 0 = nothing received yet
};
HrPacket lastHrPacket();

void begin();
void task(void* arg);

// Fills KIND_COUNT entries, one per kind, in Kind order.
void links(Link* out);

// Drop the live link for one kind. The pairing is untouched, so the task will
// re-hunt and reconnect within seconds — use forget() to make it stick.
// Returns false if that kind wasn't connected.
bool disconnect(int kind);

// Sensors screen support
void setScanAlways(bool on);            // keep scanning while the UI is open
void noteActivity();                    // user interacted — scan for a while
int getCandidates(Candidate* out, int maxOut);
void pairCandidate(const char* addr);   // saves for every kind it advertises
void forget(const char* addr);          // clear one paired address
void forgetAll();

// True while a paired-but-missing sensor is being hunted — a scan is running or
// a connect attempt is in flight. power_mgmt keeps the radio out of sleep for
// the duration; without that the hunt does not find anything (see the note in
// power_mgmt.cpp).
bool radioBusy();

// True while any sensor LINK is up. power_mgmt holds light sleep off for the
// duration — an established sensor connection does not survive the CPU
// sleeping any better than the phone's does (see the 2026-08-21 note in
// power_mgmt.cpp).
bool anyConnected();

}
