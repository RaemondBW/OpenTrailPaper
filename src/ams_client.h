#pragma once

#include <stdint.h>

// GATT client for Apple Media Service — the service the IPHONE publishes to
// BLE accessories, carrying now-playing metadata and transport/volume control
// for WHATEVER app is playing system-wide (Spotify included). This is the BLE
// sibling of the AVRCP a car stereo uses, and it is how watches and bike
// computers do "music from any app" against an iPhone.
//
// Runs as a GATT client over the SAME connection the companion app opens to
// our server (GATT roles are independent of who connected to whom). AMS
// requires an encrypted, bonded link, so the first connection after this
// ships pops one pairing dialog on the phone; declining it just leaves the
// music page on the app-fed path (Apple Music only).
//
// No album art here — AMS is strings only. Art keeps arriving from the app
// over the media characteristic, and the title-match logic in media.cpp
// already drops it when AMS reports a different track.
//
// All NimBLE calls are made from the BLE server task (tick()), never from
// UI-task or callback context.
namespace ams {

// Server-connection lifecycle, called from ble_server's callbacks.
void onConnect(uint16_t connHandle);
void onDisconnect(uint16_t connHandle);
void onSecured(uint16_t connHandle, bool encrypted);

// Drive the state machine (secure -> discover -> subscribe). Call from the
// server task loop.
void tick();

// True while entity updates are flowing — the phone-app meta path defers to
// AMS whenever this is live.
bool active();

// Send a MediaCmd (media_state.h) via AMS Remote Command. Returns false when
// AMS isn't up (caller falls back to notifying the companion app). Server
// task only.
bool sendCommand(uint8_t mediaCmd);

}  // namespace ams
