#pragma once
#include <stdint.h>

// Task-owned policy. Actual intervals are in BLE's 1.25 ms units. There is no
// GPS input: quiet metadata, location seeds and media buttons do not need a
// bulk-transfer interval. A request never substitutes for measured parameters.
class BleIntervalPolicy {
public:
    enum class Request { None, Fast, Relaxed };
    void reset(uint32_t now) {
        connectedAt = lastRequest = now;
        slow = false;
        attempts = 0;
    }
    Request update(uint32_t now, uint32_t bulkIdleMs, bool bulkActive,
                   uint16_t interval) {
        bool desired = !bulkActive && now - connectedAt >= 15000 && bulkIdleMs >= 8000;
        if (desired != slow) { slow = desired; attempts = 0; }
        bool matches = slow ? interval >= 120 && interval <= 240
                            : interval >= 12 && interval <= 24;
        if (matches) { attempts = 0; return Request::None; }
        // Give initial discovery time to settle; avoid repeatedly nagging a
        // central that rejects/overrides requests. A new mode/link gets retries.
        uint32_t waitMs = attempts ? 60000 : slow ? 15000 : 3000;
        if (attempts >= 3 || now - lastRequest < waitMs) return Request::None;
        lastRequest = now;
        ++attempts;
        return slow ? Request::Relaxed : Request::Fast;
    }
    bool wantsRelaxed() const { return slow; }
    unsigned requestAttempts() const { return attempts; }
private:
    uint32_t connectedAt = 0, lastRequest = 0;
    bool slow = false;
    unsigned attempts = 0;
};
