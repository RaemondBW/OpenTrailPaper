#pragma once

#include <stddef.h>
#include "dash_layout.h"

// SD-backed storage for the dashboard layout (dash_layout.h holds the format).
//
// The config lives in a plain text file the rider can edit directly on the card:
//
//     /config/dashboard.cfg
//
// A file rather than NVS because that is what makes it editable without the app
// — mount the card over USB, change a line, eject. The app writes the same file
// over BLE, so both routes end at one artifact and there is no second source of
// truth to reconcile.
namespace dash_config {

// Read the card and apply it. Falls back to dashDefaultLayout() when the file is
// missing, unreadable or contains nothing usable — a device with no config must
// still show a dashboard. Safe to call before the SD is mounted (it just keeps
// the default). Call again after a rescan.
void begin();

// The layout the renderer should use. Never empty.
const DashLayout& current();

// Replace the layout, persist it to the card, and take effect on the next frame.
// Returns false if the text parsed to nothing (the current layout is kept) or
// the write failed; `out` receives a short reason for the log / the app.
bool applyText(const char* text, const char** reason);

// Serialize the current layout, for the app to read back. Returns bytes written.
size_t currentText(char* out, size_t cap);

}  // namespace dash_config
