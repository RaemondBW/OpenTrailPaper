#include "usb_storage.h"

#include <Arduino.h>
#include <SD.h>
#include "USB.h"
#include "USBMSC.h"

#include "config.h"
#include "settings.h"
#include "diag.h"

namespace {

USBMSC msc;
volatile bool g_hostActive = false;     // computer has the drive mounted
volatile bool g_reclaimPending = false; // host gone -> remount FAT (in poll)
volatile uint32_t g_lastActivityMs = 0;
bool g_ready = false;
bool g_driveEnabled = true;             // SD exposed to a host as a USB drive

// Read/write whole 512-byte sectors straight off the SD card. TinyUSB passes
// sector-aligned requests (offset 0, bufsize a multiple of 512).
int32_t onRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    (void)offset;
    g_hostActive = true;
    g_lastActivityMs = millis();
    uint8_t* buf = (uint8_t*)buffer;
    uint32_t count = bufsize / 512;
    for (uint32_t i = 0; i < count; ++i) {
        if (!SD.readRAW(buf + i * 512, lba + i)) return -1;
    }
    return count * 512;
}

int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    (void)offset;
    g_hostActive = true;
    g_lastActivityMs = millis();
    uint32_t count = bufsize / 512;
    for (uint32_t i = 0; i < count; ++i) {
        if (!SD.writeRAW(buffer + i * 512, lba + i)) return -1;
    }
    return count * 512;
}

// SCSI START/STOP UNIT: the host issues this on mount (start) and eject.
bool onStartStop(uint8_t power, bool start, bool load_eject) {
    (void)power;
    if (start) { g_hostActive = true; g_lastActivityMs = millis(); }
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

// Called from ONE task (~1 Hz) so the SD remount never races another SD user.
// Keeps hostActive() true until the reclaim completes, so the firmware stays off
// the SD the whole time the computer owns it.
void poll() {
    if (!g_ready) return;
    if (g_reclaimPending) {          // set by a real unplug or a clean eject
        g_reclaimPending = false;
        SD.end();
        SD.begin(BOARD_SD_CS);       // remount FAT with the computer's changes
        g_hostActive = false;
        diag::log("usb storage: host released, SD reclaimed");
    }
}

}  // namespace usb_storage
