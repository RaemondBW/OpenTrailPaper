#pragma once

// Exposes the SD card to a host computer as a USB Mass Storage drive. The
// ESP32-S3 native USB runs a composite CDC (serial, for flashing) + MSC device,
// so plugging into a computer mounts the SD card as removable storage.
//
// CRITICAL: the firmware and the computer must never write the SD concurrently
// (FAT corruption). While a host has the drive mounted, hostActive() is true and
// the firmware suspends its own SD use (recording, logging, map/ota commits).

namespace usb_storage {

// Call after the SD card is mounted (SD.begin succeeded).
void begin();

// True while a host computer has the drive mounted — firmware SD access paused.
bool hostActive();

// Call periodically from ONE task (~1 Hz). Handles reclaiming the SD (remount)
// when the host disconnects, without racing other SD users.
void poll();

}
