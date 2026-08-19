#pragma once

#include <stddef.h>
#include "media_state.h"

// Device-side store for the phone's now-playing state (the MUSIC dashboard
// page). ble_server.cpp writes it as media packets arrive; the UI task reads
// it to render. All mutation happens on the BLE server task; the UI only ever
// reads a snapshot, so a missed frame during an update costs one repaint, not
// a tear the rider can see.
namespace media {

// Snapshot for the renderer. The art pointer stays valid until the next
// commitArt()/clear(), which both happen on the server task — the UI task
// consumes the snapshot within one frame, long before either can run again.
const MediaState& get();

// Bumped on every visible change (metadata, art, clear), so the UI task knows
// to repaint without polling field-by-field.
uint32_t version();

// --- called from ble_server (server task / callbacks) -----------------------

// Metadata packet: playing flag + progress + the three strings.
void setMeta(bool playing, uint16_t posSec, uint16_t durSec,
             const char* title, const char* artist, const char* album);

// Album art arrives as 8-bit grayscale, streamed in chunks, then committed.
// beginArt rejects silly sizes (cap 320x320) and allocates from PSRAM.
bool beginArt(int w, int h);
void artData(const uint8_t* data, size_t len);
// Dither the received grayscale to the panel's 5 usable tones and publish it.
// Runs on the SERVER TASK (deferred, like mapCommit) — Floyd-Steinberg over
// ~100 KB is far too much work for a BLE callback.
void commitArt();

// Optimistic play/pause flip from the UI task, so the button answers on the
// glass immediately; the phone's next metadata report corrects any disagreement.
void toggleLocal();

// Phone gone or player gone: drop everything, free the art.
void clear();

}  // namespace media
