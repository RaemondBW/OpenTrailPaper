#pragma once

// Starred Strava segments on the device: storage and hand-off. The format and
// the matcher are in segment_match.h (host-safe); this is the device side.
//
// The app sends the whole set as one blob over BLE (ble_server.cpp, the route
// characteristic's 0x09..0x0C ops). submit() validates it, keeps it on the SD
// card as /segments/starred.bin (so it survives a reboot) and parks the parsed
// set for the UI task, which owns the live copy and the matcher and adopts the
// new set at its next pass (takePending). Nothing here touches the matcher.

#include <cstddef>
#include <cstdint>

#include "segment_match.h"

namespace segments {

// Loads /segments/starred.bin, if there is one, into the pending slot. Needs
// the SD mounted; call again on a late mount.
void begin();

// A complete blob from the app. Validates and parses it, saves it to the SD
// (when there is a card) and hands it to the UI task. False with `err` when
// the blob is rejected — the previous set stays. SD work: call from the
// server task, not a BLE callback.
bool submit(const uint8_t* blob, size_t len, const char** err);

// Segments in the set most recently accepted (or loaded at boot); -1 before
// any. For the app's "N on device".
int count();

// UI task: swap a newly accepted set into `live`. True when it did — the
// caller re-attaches its matcher.
bool takePending(seg::SegmentDb& live);

}  // namespace segments
