#include <Arduino.h>
#include <SD.h>
#include <atomic>
#include "sd_diagnostics.h"
#include "sd_bus.h"
#include "diag.h"
#include "ride_recorder.h"
#include "usb_storage.h"
#include "ble_server.h"

namespace {
std::atomic<unsigned> requested{0};
std::atomic<bool> reportRequested{false};
uint32_t endMs = 0, nextMs = 0, sequence = 0;
uint32_t passed = 0, failed = 0, skipped = 0, initialSleep = 0;
uint32_t previousSleep = 0, afterSleepPassed = 0;
uint32_t afterSleepPhonePassed = 0, finalSleepCalls = 0;
unsigned long previousPhoneId = 0;
bool previousPhoneUp = false, everStarted = false;
bool running = false;

void report() {
    uint32_t slept, rejected; uint64_t us;
    power_mgmt::sleepStats(slept, rejected, us);
    diag::log("sdtest: %s pass=%lu fail=%lu skipped=%lu sleep_calls=%lu after_sleep_pass=%lu phone_sleep_pass=%lu remaining_s=%lu",
              running ? "running" : everStarted ? "done" : "idle",
              (unsigned long)passed, (unsigned long)failed, (unsigned long)skipped,
              (unsigned long)(running ? slept - initialSleep : finalSleepCalls),
              (unsigned long)afterSleepPassed, (unsigned long)afterSleepPhonePassed,
              (unsigned long)(running && (int32_t)(endMs - millis()) > 0 ? (endMs - millis()) / 1000 : 0));
}
}
void sd_diagnostics::status() { reportRequested = true; }
void sd_diagnostics::start(unsigned seconds) {
    if (seconds < 30 || seconds > 1800) {
        diag::log("sdtest: duration must be 30..1800 seconds"); return;
    }
    requested = seconds;
}
void sd_diagnostics::tick() {
    unsigned seconds = requested.exchange(0);
    uint32_t now = millis();
    if (seconds) {
        endMs = now + seconds * 1000;
        nextMs = now + 15000; // time to unplug USB and release its guard
        passed = failed = skipped = 0;
        uint32_t rejected; uint64_t us;
        power_mgmt::sleepStats(initialSleep, rejected, us);
        previousSleep = initialSleep; afterSleepPassed = 0;
        afterSleepPhonePassed = finalSleepCalls = 0;
        previousPhoneId = ble_server::phoneConnectionId();
        previousPhoneUp = ble_server::isPhoneConnected();
        everStarted = true;
        running = true;
        diag::log("sdtest: armed %us; unplug USB; checks every 5s after 15s", seconds);
    }
    if (reportRequested.exchange(false)) report();
    if (!running) return;
    if ((int32_t)(now - endMs) >= 0) {
        uint32_t slept, rejected; uint64_t us;
        power_mgmt::sleepStats(slept, rejected, us);
        finalSleepCalls = slept - initialSleep;
        running = false;
        report();
        diag::flushToSD();
        return;
    }
    if ((int32_t)(now - nextMs) < 0) return;
    nextMs = now + 5000;
    if (!ride_recorder::sdMounted() || ride_recorder::isRecording() || usb_storage::hostActive()) {
        ++skipped;
        diag::log("sdtest: skipped (mounted=%d recording=%d host=%d)",
                  ride_recorder::sdMounted(), ride_recorder::isRecording(), usb_storage::hostActive());
        return;
    }
    uint32_t sleepCalls, rejected; uint64_t sleepUs;
    power_mgmt::sleepStats(sleepCalls, rejected, sleepUs);
    uint32_t sleepDelta = sleepCalls - previousSleep;
    previousSleep = sleepCalls;
    unsigned long phoneId = ble_server::phoneConnectionId();
    bool phoneUp = ble_server::isPhoneConnected();
    bool samePhone = previousPhoneUp && phoneUp && phoneId == previousPhoneId;
    previousPhoneId = phoneId; previousPhoneUp = phoneUp;
    sdLock();
    // Recheck after acquiring the bus; never disturb recording or USB ownership.
    if (!ride_recorder::sdMounted() || ride_recorder::isRecording()) { sdUnlock(); ++skipped; return; }
    char path[64];
    snprintf(path, sizeof(path), "/logs/sdtest-%08lx-%08lx.tmp",
             (unsigned long)now, (unsigned long)++sequence);
    bool ok = SD.exists("/logs") || SD.mkdir("/logs");
    bool created = false;
    uint8_t expected[512], actual[512];
    for (unsigned i = 0; i < sizeof(expected); ++i) expected[i] = (i * 37 + sequence) & 255;
    // Do not overwrite an existing file, even after a reset/millis wrap.
    if (ok && !SD.exists(path)) {
        File f = SD.open(path, FILE_WRITE);
        created = (bool)f;
        ok = f && f.write(expected, sizeof(expected)) == sizeof(expected);
        if (f) { f.flush(); f.close(); }
        if (ok) {
            f = SD.open(path, FILE_READ);
            ok = f && f.size() == sizeof(expected) &&
                 f.read(actual, sizeof(actual)) == sizeof(actual) &&
                 memcmp(expected, actual, sizeof(actual)) == 0;
            if (f) f.close();
        }
    } else ok = false;
    if (created && !SD.remove(path)) ok = false;
    sdUnlock();
    if (ok) {
        ++passed;
        if (sleepDelta) {
            ++afterSleepPassed;
            if (samePhone) ++afterSleepPhonePassed;
        }
    } else ++failed;
    diag::drainDriverLogs();
    diag::log("sdtest: write/close/reopen/compare/remove %s sample=%lu preceding_sleep_calls=%lu phone_same_link=%d phone_id=%lu",
              ok ? "PASS" : "FAILED", (unsigned long)(passed + failed), (unsigned long)sleepDelta,
              samePhone, phoneId);
    if (!ok) diag::checkpoint("SD battery test failure");
}
