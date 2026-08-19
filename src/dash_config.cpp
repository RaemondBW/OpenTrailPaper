#include "dash_config.h"

#include <Arduino.h>
#include <SD.h>
#include <string.h>

#include "sd_bus.h"
#include "usb_storage.h"
#include "diag.h"

namespace {

constexpr char CONFIG_DIR[] = "/config";
constexpr char CONFIG_PATH[] = "/config/dashboard.cfg";

// The whole file is small (the default serializes to ~200 bytes) and it is read
// once at boot and written only when the rider changes it, so a fixed buffer is
// simpler and safer here than anything dynamic.
// Sized for DASH_MAX_PAGES full pages plus the header — the old 640 held one.
constexpr size_t TEXT_MAX = 2048;

DashPages g_pages = dashDefaultPages();

bool writeFile(const char* text) {
    if (usb_storage::hostActive()) return false;   // the card is not ours
    sdLock();
    if (!SD.exists(CONFIG_DIR)) SD.mkdir(CONFIG_DIR);
    SD.remove(CONFIG_PATH);
    File f = SD.open(CONFIG_PATH, FILE_WRITE);
    bool ok = (bool)f;
    if (ok) {
        size_t len = strlen(text);
        ok = f.write((const uint8_t*)text, len) == len;
        f.close();
    }
    if (!ok) SD.remove(CONFIG_PATH);   // never leave a half-written config
    sdUnlock();
    return ok;
}

}  // namespace

namespace dash_config {

void begin() {
    if (usb_storage::hostActive()) return;
    char text[TEXT_MAX];
    size_t got = 0;
    sdLock();
    File f = SD.open(CONFIG_PATH, FILE_READ);
    if (f) {
        got = f.read((uint8_t*)text, sizeof(text) - 1);
        f.close();
    }
    sdUnlock();
    if (!got) {
        // No config yet. Write the default out so the file EXISTS to be edited —
        // discovering the format is the hard part of a hand-edited config, and a
        // commented example on the card is the whole answer.
        g_pages = dashDefaultPages();
        char out[TEXT_MAX];
        if (dashSerializePages(g_pages, out, sizeof(out)) && writeFile(out))
            diag::log("dash: wrote default %s", CONFIG_PATH);
        return;
    }
    text[got] = 0;

    DashPages parsed;
    if (dashParsePages(text, parsed)) {
        g_pages = parsed;
        diag::log("dash: layout loaded (%d page%s, %d fields on page 1)",
                  parsed.count, parsed.count == 1 ? "" : "s",
                  parsed.pages[0].layout.count);
    } else {
        // Present but unusable. Keep the default and SAY SO — a rider who has
        // just edited the file needs to know it was rejected, and a blank-looking
        // dashboard with no explanation is how that goes unnoticed for a ride.
        g_pages = dashDefaultPages();
        diag::log("dash: %s has no usable fields — using the default", CONFIG_PATH);
    }
}

const DashPages& pages() { return g_pages; }
int pageCount() { return g_pages.count; }
const DashPage& page(int idx) {
    if (idx < 0 || idx >= g_pages.count) idx = 0;
    return g_pages.pages[idx];
}

const DashLayout& current() {
    for (int i = 0; i < g_pages.count; ++i)
        if (g_pages.pages[i].kind == DP_FIELDS) return g_pages.pages[i].layout;
    return dashDefaultLayout();
}

bool applyText(const char* text, const char** reason) {
    static const char* kNoFields = "no usable fields";
    static const char* kWriteFailed = "SD write failed";
    if (reason) *reason = nullptr;

    DashPages parsed;
    if (!dashParsePages(text, parsed)) {
        if (reason) *reason = kNoFields;
        return false;
    }
    // Apply first, persist second: a card that is busy (host owns it, recorder
    // mid-write) should not stop the rider seeing the change they just made.
    // The write is retried at the next begin() only in the sense that a failed
    // save means the file still holds the previous layout — which is reported.
    g_pages = parsed;
    if (!writeFile(text)) {
        if (reason) *reason = kWriteFailed;
        return false;
    }
    diag::log("dash: layout updated from the app (%d page%s)", parsed.count,
              parsed.count == 1 ? "" : "s");
    return true;
}

size_t currentText(char* out, size_t cap) {
    return dashSerializePages(g_pages, out, cap);
}

}  // namespace dash_config
