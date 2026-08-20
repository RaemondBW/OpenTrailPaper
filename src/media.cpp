#include "media.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "diag.h"

namespace {

MediaState g_state;
volatile uint32_t g_version = 0;
// millis() of the last AMS attribute update; 0 = never. Gates the app's
// metadata feed (Apple Music only) so it can't fight AMS over the same page.
uint32_t g_amsMs = 0;
constexpr uint32_t AMS_LIVE_MS = 20000;

void bump() { g_version = g_version + 1; }

// Incoming art staging (8-bit grayscale from the phone).
uint8_t* g_rx = nullptr;
size_t g_rxLen = 0, g_rxCap = 0;
int g_rxW = 0, g_rxH = 0;

// Published art (panel tones, one byte per pixel).
uint8_t* g_art = nullptr;

constexpr int ART_MAX = 320;   // the content column is 492 px; 320 is plenty

void freeRx() {
    if (g_rx) { heap_caps_free(g_rx); g_rx = nullptr; }
    g_rxLen = g_rxCap = 0;
    g_rxW = g_rxH = 0;
}

void copyStr(char* dst, size_t cap, const char* src) {
    if (!src) src = "";
    snprintf(dst, cap, "%s", src);
}

}  // namespace

namespace media {

const MediaState& get() { return g_state; }
uint32_t version() { return g_version; }

void dropArt() {
    if (g_art) { heap_caps_free(g_art); g_art = nullptr; }
    g_state.art = nullptr;
    g_state.artW = g_state.artH = 0;
}

bool amsLive() { return g_amsMs && millis() - g_amsMs < AMS_LIVE_MS; }

void setMeta(bool playing, uint16_t posSec, uint16_t durSec,
             const char* title, const char* artist, const char* album) {
    // AMS is authoritative while it flows: it sees every player, the app path
    // sees only Apple Music, and two writers disagreeing about "the" track
    // makes the page flicker between them.
    if (amsLive()) return;
    // A track change with the art from the last track under it is a lie worth
    // preventing: the phone always follows metadata with new art (or none),
    // so stale art is dropped the moment the title stops matching.
    if (strncmp(g_state.title, title ? title : "", sizeof(g_state.title)) != 0)
        dropArt();
    g_state.present = true;
    g_state.playing = playing;
    g_state.posSec = posSec;
    g_state.durSec = durSec;
    g_state.posAtMs = millis();
    copyStr(g_state.title, sizeof(g_state.title), title);
    copyStr(g_state.artist, sizeof(g_state.artist), artist);
    copyStr(g_state.album, sizeof(g_state.album), album);
    bump();
}

bool beginArt(int w, int h) {
    freeRx();
    if (w <= 0 || h <= 0 || w > ART_MAX || h > ART_MAX) return false;
    g_rxCap = (size_t)w * h;
    g_rx = (uint8_t*)heap_caps_malloc(g_rxCap, MALLOC_CAP_SPIRAM);
    if (!g_rx) { g_rxCap = 0; return false; }
    g_rxW = w;
    g_rxH = h;
    g_rxLen = 0;
    return true;
}

void artData(const uint8_t* data, size_t len) {
    if (!g_rx) return;
    if (g_rxLen + len > g_rxCap) len = g_rxCap - g_rxLen;
    memcpy(g_rx + g_rxLen, data, len);
    g_rxLen += len;
}

void commitArt() {
    if (!g_rx || g_rxLen < g_rxCap) { freeRx(); return; }

    // Floyd-Steinberg down to the tones this panel actually renders. Nibbles
    // 0x4-0xE read as white on this glass (see the panel-greys note in
    // ui_render.h), so the palette is black, the three dark greys, and white —
    // with their APPROXIMATE reflectances as thresholds, not an even spread:
    // crushing 0x11-0x33 into the midtones is what keeps faces recognisable.
    static const uint8_t kTone[5] = {0x00, 0x11, 0x22, 0x33, 0xFF};
    static const int kLum[5] = {0, 70, 120, 170, 255};

    uint8_t* out = (uint8_t*)heap_caps_malloc(g_rxCap, MALLOC_CAP_SPIRAM);
    if (!out) { freeRx(); return; }

    // Error rows in internal RAM (2 x 322 ints), serpentine-free simple scan.
    const int W = g_rxW, H = g_rxH;
    int16_t* err = (int16_t*)calloc(2 * (W + 2), sizeof(int16_t));
    if (!err) { heap_caps_free(out); freeRx(); return; }
    int16_t* cur = err + 1;
    int16_t* nxt = err + (W + 2) + 1;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            int v = g_rx[y * W + x] + cur[x];
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            int best = 0, bestD = 999;
            for (int k = 0; k < 5; ++k) {
                int d = v - kLum[k];
                if (d < 0) d = -d;
                if (d < bestD) { bestD = d; best = k; }
            }
            out[y * W + x] = kTone[best];
            int e = v - kLum[best];
            cur[x + 1] += e * 7 / 16;
            nxt[x - 1] += e * 3 / 16;
            nxt[x]     += e * 5 / 16;
            nxt[x + 1] += e * 1 / 16;
        }
        int16_t* t = cur; cur = nxt; nxt = t;
        memset(nxt - 1, 0, (W + 2) * sizeof(int16_t));
    }
    free(err);

    if (g_art) heap_caps_free(g_art);
    g_art = out;
    g_state.art = g_art;
    g_state.artW = W;
    g_state.artH = H;
    diag::log("media: art %dx%d dithered", W, H);
    freeRx();
    bump();
}

void amsTitle(const char* title) {
    g_amsMs = millis();
    if (strncmp(g_state.title, title ? title : "",
                sizeof(g_state.title)) != 0) {
        dropArt();
        snprintf(g_state.title, sizeof(g_state.title), "%s",
                 title ? title : "");
        // A fresh track: elapsed restarts unless AMS says otherwise in the
        // PlaybackInfo that follows a track change.
        g_state.posSec = 0;
        g_state.posAtMs = millis();
    }
    g_state.present = true;
    bump();
}

void amsArtist(const char* artist) {
    g_amsMs = millis();
    snprintf(g_state.artist, sizeof(g_state.artist), "%s",
             artist ? artist : "");
    g_state.present = true;
    bump();
}

void amsAlbum(const char* album) {
    g_amsMs = millis();
    snprintf(g_state.album, sizeof(g_state.album), "%s", album ? album : "");
    g_state.present = true;
    bump();
}

void amsPlayback(bool playing, uint16_t posSec) {
    g_amsMs = millis();
    g_state.present = true;
    g_state.playing = playing;
    g_state.posSec = posSec;
    g_state.posAtMs = millis();
    bump();
}

void amsDuration(uint16_t durSec) {
    g_amsMs = millis();
    g_state.durSec = durSec;
    bump();
}

void clearFromApp() {
    if (amsLive()) return;   // the app can't blank a track AMS still reports
    clear();
}

void toggleLocal() {
    if (!g_state.present) return;
    // Freeze the shown position at the moment of the press: pausing must stop
    // the bar, and resuming must not credit the paused time as playback.
    uint32_t now = millis();
    if (g_state.playing && g_state.durSec > 0) {
        uint32_t adv = (now - g_state.posAtMs) / 1000;
        uint32_t p = g_state.posSec + adv;
        g_state.posSec = p > g_state.durSec ? g_state.durSec : (uint16_t)p;
    }
    g_state.posAtMs = now;
    g_state.playing = !g_state.playing;
    bump();
}

void clear() {
    freeRx();
    if (g_art) { heap_caps_free(g_art); g_art = nullptr; }
    bool was = g_state.present;
    g_state = MediaState{};
    if (was) bump();
}

}  // namespace media
