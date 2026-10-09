#pragma once

#include <stddef.h>
#include "media_state.h"

// Device-side store for the phone's now-playing state (the MUSIC dashboard
// page). Two feeds write it, one page reads it:
//
//   - AMS (ams_client.cpp): the iPhone's own Apple Media Service — metadata,
//     playback state and duration for whatever app is playing, Spotify
//     included. Authoritative whenever its updates are flowing.
//   - The companion app (ble_server media characteristic): metadata (Apple
//     Music only — all iOS lets an app see) and ALBUM ART, which AMS does not
//     carry. Its metadata is ignored while AMS is live; its art is always
//     welcome, and stale art is dropped on any title change — including art
//     still in flight when the title changed, which never gets published.
//
// All mutation happens on the BLE server task / NimBLE callbacks; the UI only
// ever reads a snapshot, so a missed frame during an update costs one
// repaint, not a tear the rider can see.
namespace media {

// Snapshot for the renderer. The art pointer stays valid until the next
// commitArt()/clear(), which both happen on the server task — the UI task
// consumes the snapshot within one frame, long before either can run again.
const MediaState& get();

// Bumped on every visible change (metadata, art, clear), so the UI task knows
// to repaint without polling field-by-field.
uint32_t version();

// --- companion-app feed (ble_server callbacks / server task) ----------------

// Metadata packet from the app. Ignored while AMS is live (see amsNote).
void setMeta(bool playing, uint16_t posSec, uint16_t durSec,
             const char* title, const char* artist, const char* album);

// Album art arrives as 8-bit grayscale, streamed in chunks, then committed.
// beginArt rejects silly sizes (cap 320x320) and allocates from PSRAM.
bool beginArt(int w, int h);
// Or as tone indices (0-4) the phone already dithered, base-5 packed three
// pixels per byte, first pixel in the lowest digit: a third of the bytes over
// the air, and no dither here.
bool beginToneArt(int w, int h);
void artData(const uint8_t* data, size_t len);
// Dither the received grayscale to the panel's 5 usable tones (or unpack tone
// art) and publish it. Runs on the SERVER TASK (deferred, like mapCommit) —
// Floyd-Steinberg over ~100 KB is far too much work for a BLE callback.
void commitArt();

// --- AMS feed (ams_client.cpp, NimBLE host task) ----------------------------

// Partial updates, one attribute at a time, exactly as AMS notifies them.
// Any change stamps the AMS-live clock that gates setMeta().
void amsTitle(const char* title);     // a NEW title also drops the shown art
void amsArtist(const char* artist);
void amsAlbum(const char* album);
void amsPlayback(bool playing, uint16_t posSec);
void amsDuration(uint16_t durSec);
// True within a grace window of the last AMS update.
bool amsLive();
// Bumped whenever AMS changes the title, artist or album (0 = AMS has never
// reported a track), and when that last happened — the server task forwards
// the track to the companion app (0xA2) once the burst settles, so an iPhone
// app can find art for players it cannot see itself.
uint32_t amsTrackVersion();
uint32_t amsTrackChangedMs();

// Optimistic play/pause flip from the UI task, so the button answers on the
// glass immediately; the next real report corrects any disagreement.
void toggleLocal();

// Phone gone or player gone: drop everything, free the art. `fromApp` clears
// only if AMS isn't live — the app saying "Apple Music stopped" must not
// blank a Spotify track AMS is still reporting.
void clear();
void clearFromApp();

}  // namespace media
