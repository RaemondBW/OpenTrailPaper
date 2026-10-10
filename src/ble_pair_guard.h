#pragma once

// NimBLE host knobs the Arduino wrapper does not expose, for ble_server's
// fixed pairing. C, because they live behind the host's private headers.
//
//  * ble_pair_guard_harden(): Secure Connections Only mode and authenticated
//    CCCDs. Call once, after NimBLEDevice::init() and BEFORE the GATT server
//    starts (the CCCD permission is read when the descriptors are registered).
//  * ble_pair_guard_install(): wrap one server connection's GAP callback so a
//    pairing request from a peer we already hold a bond for can be refused.
//    NimBLE-Arduino's own handler deletes that bond and lets the pairing run —
//    which is the paired phone's bond, gone before anything can say no.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ble_pair_guard_harden(void);

// Asked on every repeat-pairing request on a guarded link: true refuses it
// (the request is ignored — no pairing response, so the phone never reaches
// its passkey dialog). Called on the NimBLE host task.
typedef bool ble_pair_guard_refuse_fn(uint16_t conn_handle);
void ble_pair_guard_set_refuse(ble_pair_guard_refuse_fn* fn);

// Call from the server's onConnect. Returns 0, or a NimBLE error.
int ble_pair_guard_install(uint16_t conn_handle);

#ifdef __cplusplus
}
#endif
