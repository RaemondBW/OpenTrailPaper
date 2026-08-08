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
constexpr size_t TEXT_MAX = 640;

DashLayout g_layout = dashDefaultLayout();

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
        g_layout = dashDefaultLayout();
        char out[TEXT_MAX];
        if (dashSerialize(g_layout, out, sizeof(out)) && writeFile(out))
            diag::log("dash: wrote default %s", CONFIG_PATH);
        return;
    }
    text[got] = 0;

    DashLayout parsed;
    if (dashParse(text, parsed)) {
        g_layout = parsed;
        diag::log("dash: layout loaded (%d fields)", parsed.count);
    } else {
        // Present but unusable. Keep the default and SAY SO — a rider who has
        // just edited the file needs to know it was rejected, and a blank-looking
        // dashboard with no explanation is how that goes unnoticed for a ride.
        g_layout = dashDefaultLayout();
        diag::log("dash: %s has no usable fields — using the default", CONFIG_PATH);
    }
}

const DashLayout& current() { return g_layout; }

bool applyText(const char* text, const char** reason) {
    static const char* kNoFields = "no usable fields";
    static const char* kWriteFailed = "SD write failed";
    if (reason) *reason = nullptr;

    DashLayout parsed;
    if (!dashParse(text, parsed)) {
        if (reason) *reason = kNoFields;
        return false;
    }
    // Apply first, persist second: a card that is busy (host owns it, recorder
    // mid-write) should not stop the rider seeing the change they just made.
    // The write is retried at the next begin() only in the sense that a failed
    // save means the file still holds the previous layout — which is reported.
    g_layout = parsed;
    if (!writeFile(text)) {
        if (reason) *reason = kWriteFailed;
        return false;
    }
    diag::log("dash: layout updated from the app (%d fields)", parsed.count);
    return true;
}

size_t currentText(char* out, size_t cap) {
    return dashSerialize(g_layout, out, cap);
}

}  // namespace dash_config
