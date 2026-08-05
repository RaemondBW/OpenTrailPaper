#include "usb_storage.h"

#include <Arduino.h>
#include <SD.h>
#include "USB.h"
#include "USBMSC.h"

#include "config.h"
#include "settings.h"
#include "diag.h"
#include "ride_recorder.h"

namespace {

USBMSC msc;
volatile bool g_hostActive = false;     // computer has the drive mounted
volatile bool g_reclaimPending = false; // host gone -> remount FAT (in poll)
volatile uint32_t g_lastActivityMs = 0;
bool g_ready = false;
bool g_driveEnabled = true;             // SD exposed to a host as a USB drive
bool g_loggedActive = false;            // "host took the SD" logged once

// How long the drive can sit with no sector traffic at all before we take the
// card back. `hostActive` gates sdMounted(), so without this it is a latch: a
// host that goes away without ejecting or dropping VBUS — a laptop going to
// sleep, a hub that stops talking — leaves the device reporting "no SD" until it
// is rebooted, which is exactly the failure this was written to fix.
//
// Long on purpose. A host that still has the volume mounted may write again, and
// remounting under it risks the corruption the eject-only rule was protecting
// against; five silent minutes is well past any normal filesystem chatter.
constexpr uint32_t IDLE_RECLAIM_MS = 5 * 60 * 1000;

// The host can only own the card if we are actually offering it. With the drive
// disabled the media reads as "not present", so nothing a host says can mean it
// has our filesystem mounted — and latching anyway stranded the firmware with no
// way back, because a volume that was never mounted can never be ejected either,
// which is the one path that used to clear this.
inline void noteHostTraffic() {
    if (!g_driveEnabled) return;
    g_hostActive = true;
    g_lastActivityMs = millis();
}

// Read/write whole 512-byte sectors straight off the SD card. TinyUSB passes
// sector-aligned requests (offset 0, bufsize a multiple of 512).
int32_t onRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    (void)offset;
    noteHostTraffic();
    uint8_t* buf = (uint8_t*)buffer;
    uint32_t count = bufsize / 512;
    for (uint32_t i = 0; i < count; ++i) {
        if (!SD.readRAW(buf + i * 512, lba + i)) return -1;
    }
    return count * 512;
}

int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    (void)offset;
    noteHostTraffic();
    uint32_t count = bufsize / 512;
    for (uint32_t i = 0; i < count; ++i) {
        if (!SD.writeRAW(buffer + i * 512, lba + i)) return -1;
    }
    return count * 512;
}

// SCSI START/STOP UNIT: the host issues this on mount (start) and eject.
bool onStartStop(uint8_t power, bool start, bool load_eject) {
    (void)power;
    // A START is NOT evidence the host mounted anything — a charger or any
    // machine that merely enumerates the device sends one too, and with the
    // drive disabled it can never go on to mount a volume. Only real sector
    // traffic counts as the host owning the card. Latching here is what left the
    // device reporting "no SD" after being plugged into something that never
    // used it, with no eject to ever release it again.
    if (start) g_lastActivityMs = millis();
    if (!start && load_eject) g_reclaimPending = true;   // ejected
    return true;
}

void usbEvent(void*, esp_event_base_t base, int32_t id, void*) {
    if (base != ARDUINO_USB_EVENTS) return;
    // Reclaim ONLY on a real unplug (VBUS loss). NOT on suspend (host sleep) or
    // idle — macOS keeps the volume mounted then, and yanking the SD back while
    // it's still mounted risks corruption. A clean eject (onStartStop) also
    // reclaims, so the user can hand control back without unplugging.
    if (id == ARDUINO_USB_STOPPED_EVENT) {
        g_reclaimPending = true;
    }
}

}  // namespace

namespace usb_storage {

void begin() {
    uint32_t sectors = SD.numSectors();
    if (sectors == 0) return;   // no card
    USB.onEvent(usbEvent);
    msc.vendorID("BikeGPS");
    msc.productID("SD Card");
    msc.productRevision("1.0");
    msc.onRead(onRead);
    msc.onWrite(onWrite);
    msc.onStartStop(onStartStop);
    g_driveEnabled = settings::usbDrive();
    msc.mediaPresent(g_driveEnabled);   // "no media" when the drive is disabled
    msc.begin(sectors, 512);
    USB.begin();
    g_ready = true;
    diag::log("usb storage: MSC ready, drive %s (%u sectors)",
              g_driveEnabled ? "ON" : "OFF", (unsigned)sectors);
}

// Toggle whether the host sees the SD. Turning it off presents "no media" so
// the drive vanishes and the firmware keeps the card; also reclaims the SD (in
// poll) in case the host had written to it.
void setDriveEnabled(bool on) {
    if (!g_ready || on == g_driveEnabled) return;
    g_driveEnabled = on;
    msc.mediaPresent(on);
    if (!on) g_reclaimPending = true;   // take the SD back / remount FAT
    diag::log("usb storage: drive turned %s", on ? "ON" : "OFF");
}

bool driveEnabled() { return g_driveEnabled; }

bool hostActive() { return g_hostActive; }

// Called from ONE task (~1 Hz). Keeps hostActive() true until the reclaim
// completes, so the firmware stays off the SD the whole time the computer owns
// it.
void poll() {
    if (!g_ready) return;

    // Say so in the log the moment the host takes the card. "No SD" on screen
    // means two completely different things — the card failed to mount, or a
    // computer owns it — and without this line the log can't tell them apart.
    if (g_hostActive && !g_loggedActive) {
        g_loggedActive = true;
        diag::log("usb storage: host has the SD (device reports no SD until released)");
    }

    // Nothing from the host for a long time and no eject: assume it's gone.
    // Skipped during a ride — recording is the one time being wrong about this
    // is expensive, and the ride is writing to the card anyway.
    if (g_hostActive && !g_reclaimPending &&
        millis() - g_lastActivityMs > IDLE_RECLAIM_MS &&
        !ride_recorder::isRecording()) {
        diag::log("usb storage: no host traffic for %u s — taking the SD back",
                  (unsigned)(IDLE_RECLAIM_MS / 1000));
        g_reclaimPending = true;
    }

    if (!g_reclaimPending) return;
    // Deliberately left pending rather than done now: this used to call SD.end()
    // straight from here, with no sdLock and no regard for a ride in progress —
    // an unplug mid-ride tore the filesystem out from under the open FIT file,
    // which is precisely the interrupted transaction that wedges a card into
    // cardType=NONE. ride_recorder::remount() takes the lock and refuses during
    // a ride, so all this has to do is wait for the ride to end.
    if (ride_recorder::isRecording()) return;

    g_reclaimPending = false;
    bool ok = ride_recorder::remount("usb host released");
    g_hostActive = false;
    g_loggedActive = false;
    diag::log("usb storage: host released, SD %s", ok ? "reclaimed" : "REMOUNT FAILED");
}

}  // namespace usb_storage
