#include "radar.h"
#include <algorithm>

void radarExpire(RadarState& s, uint32_t now) {
    s.live = s.connected && s.received && uint32_t(now - s.packetMs) <= RADAR_SIGNAL_TIMEOUT_MS;
    int n = 0;
    for (int i = 0; i < s.count; ++i) {
        if (s.live && uint32_t(now - s.targets[i].seenMs) <= RADAR_TARGET_TIMEOUT_MS)
            s.targets[n++] = s.targets[i];
    }
    s.count = n;
}

bool radarIngest(RadarState& s, const uint8_t* data, size_t len, uint32_t now) {
    if (!data || len == 0 || len > 1 + 3 * RADAR_MAX_TARGETS || (len - 1) % 3) return false;
    const bool continuation = s.received && uint32_t(now - s.packetMs) < 250 &&
                              data[0] == uint8_t(s.sequence + 2);
    if ((data[0] & 0x0f) != 0x02 && !continuation) return false;
    // Expire against the OLD packet time before refreshing the heartbeat. A
    // stream coming back after an outage cannot resurrect old tracks.
    radarExpire(s, now);
    s.received = true;
    s.packetMs = now;
    s.sequence = data[0];
    s.live = s.connected;
    for (size_t p = 1; p < len; p += 3) {
        const uint8_t rawID = data[p], distance = data[p + 1];
        if (!(rawID & 0x80) || rawID == 0xfd || distance == 0xff) continue;
        const uint8_t id = rawID & 0x7f;
        int slot = 0;
        while (slot < s.count && s.targets[slot].id != id) ++slot;
        if (distance == 0) { // passed the rider
            if (slot < s.count) s.targets[slot] = s.targets[--s.count];
            continue;
        }
        if (slot == s.count) {
            if (s.count < RADAR_MAX_TARGETS) ++s.count;
            else {
                // Bounded storage: retain the nearest targets when saturated.
                slot = s.count - 1;
                if (s.targets[slot].distanceM <= distance) continue;
            }
        }
        s.targets[slot] = {id, distance, now};
        std::sort(s.targets, s.targets + s.count,
                  [](const RadarTarget& a, const RadarTarget& b) { return a.distanceM < b.distanceM; });
    }
    std::sort(s.targets, s.targets + s.count,
              [](const RadarTarget& a, const RadarTarget& b) { return a.distanceM < b.distanceM; });
    return true;
}
