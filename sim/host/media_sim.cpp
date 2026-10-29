// `media_state` on the control channel: the device's now-playing store
// (src/media.cpp) as the MUSIC page reads it — the track, playback position,
// whether AMS is feeding it, and the album art it would draw (size, a hash of
// the panel tones and how many pixels of each tone).
#include <Arduino.h>

#include "ams_client.h"
#include "json.h"
#include "media.h"

#include <sstream>

std::string mediaStateCommand(const json::Value&) {
    const MediaState m = media::get();
    const uint32_t now = millis();
    std::ostringstream o;
    o << "\"ok\":true,\"media\":{\"present\":" << (m.present ? "true" : "false")
      << ",\"playing\":" << (m.playing ? "true" : "false") << ",\"title\":" << json::esc(m.title)
      << ",\"artist\":" << json::esc(m.artist) << ",\"album\":" << json::esc(m.album)
      << ",\"position_ms\":" << (m.present ? m.positionMs(now) : 0) << ",\"duration_s\":" << m.durSec
      << ",\"rate\":" << m.rate << ",\"ams\":" << (ams::active() ? "true" : "false")
      << ",\"ams_live\":" << (media::amsLive() ? "true" : "false") << ",\"version\":" << media::version()
      << ",\"art_w\":" << m.artW << ",\"art_h\":" << m.artH;
    uint32_t hash = 2166136261u;   // FNV-1a over the published tones
    long tones[5] = {};
    const uint8_t kTone[5] = {0x00, 0x11, 0x22, 0x33, 0xFF};
    if (m.art) {
        for (int i = 0; i < m.artW * m.artH; ++i) {
            hash = (hash ^ m.art[i]) * 16777619u;
            for (int k = 0; k < 5; ++k) if (m.art[i] == kTone[k]) tones[k]++;
        }
    }
    o << ",\"art_hash\":" << (m.art ? (long long)hash : -1) << ",\"art_tones\":[";
    for (int k = 0; k < 5; ++k) o << (k ? "," : "") << tones[k];
    o << "]}";
    return o.str();
}
