#include "ams_client.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <string.h>
#include <stdlib.h>

#include "diag.h"
#include "media.h"

namespace {

// Apple Media Service, as published by every iPhone (Apple's AMS spec).
const NimBLEUUID AMS_SVC("89D3502B-0F36-433A-8EF4-C502AD55F8DC");
const NimBLEUUID AMS_REMOTE_CMD("9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2");
const NimBLEUUID AMS_ENTITY_UPD("2F7CABCE-808D-411F-9A0C-BB92BA96C102");

// AMS wire constants.
constexpr uint8_t ENTITY_PLAYER = 0, ENTITY_QUEUE = 1, ENTITY_TRACK = 2;
constexpr uint8_t PLAYER_ATTR_PLAYBACK_INFO = 1;
constexpr uint8_t TRACK_ATTR_ARTIST = 0, TRACK_ATTR_ALBUM = 1,
                  TRACK_ATTR_TITLE = 2, TRACK_ATTR_DURATION = 3;
// RemoteCommandIDs (the ones we send).
constexpr uint8_t CMD_TOGGLE = 2, CMD_NEXT = 3, CMD_PREV = 4,
                  CMD_VOL_UP = 5, CMD_VOL_DOWN = 6;

enum class State : uint8_t {
    IDLE,        // no phone connection
    SECURING,    // asked for encryption, waiting for onSecured
    DISCOVER,    // encrypted: find AMS + subscribe (tick does the work)
    RUNNING,     // entity updates flowing
    UNAVAILABLE, // phone has no AMS for us (pairing declined / not an iPhone)
};

volatile State g_state = State::IDLE;
volatile uint16_t g_conn = 0xFFFF;
volatile bool g_kick = false;       // tick() has work to do
uint32_t g_secureAskedMs = 0;
NimBLERemoteCharacteristic* g_remoteCmd = nullptr;

// Entity Update notify: [entity][attribute][flags][utf8 value...]
void onEntityUpdate(NimBLERemoteCharacteristic*, uint8_t* data, size_t len,
                    bool) {
    if (len < 3) return;
    const uint8_t entity = data[0], attr = data[1];
    char val[96];
    size_t n = len - 3 < sizeof(val) - 1 ? len - 3 : sizeof(val) - 1;
    memcpy(val, data + 3, n);
    val[n] = 0;

    if (g_state != State::RUNNING) g_state = State::RUNNING;

    if (entity == ENTITY_TRACK) {
        switch (attr) {
            case TRACK_ATTR_TITLE:  media::amsTitle(val); break;
            case TRACK_ATTR_ARTIST: media::amsArtist(val); break;
            case TRACK_ATTR_ALBUM:  media::amsAlbum(val); break;
            case TRACK_ATTR_DURATION:
                media::amsDuration((uint16_t)strtof(val, nullptr));
                break;
        }
    } else if (entity == ENTITY_PLAYER && attr == PLAYER_ATTR_PLAYBACK_INFO) {
        // "PlaybackState,PlaybackRate,ElapsedTime" — e.g. "1,1.0,143.5".
        // An empty value means no player at all.
        if (!val[0]) { media::clear(); return; }
        int playState = atoi(val);
        float elapsed = 0;
        const char* c = strchr(val, ',');
        if (c) c = strchr(c + 1, ',');
        if (c) elapsed = strtof(c + 1, nullptr);
        media::amsPlayback(playState == 1, (uint16_t)elapsed);
    }
}

}  // namespace

namespace ams {

void onConnect(uint16_t connHandle) {
    g_conn = connHandle;
    g_state = State::SECURING;
    g_secureAskedMs = 0;   // tick sends the security request (not callback ctx)
    g_kick = true;
}

void onDisconnect(uint16_t connHandle) {
    if (connHandle != g_conn) return;
    g_conn = 0xFFFF;
    g_state = State::IDLE;
    g_remoteCmd = nullptr;
    media::clear();
}

void onSecured(uint16_t connHandle, bool encrypted) {
    if (connHandle != g_conn) return;
    if (encrypted) {
        g_state = State::DISCOVER;
        g_kick = true;
    } else {
        // Pairing declined. The app-fed path still works; note it once.
        g_state = State::UNAVAILABLE;
        diag::log("ams: link not encrypted — music limited to the app feed");
    }
}

bool active() { return g_state == State::RUNNING; }

void tick() {
    if (!g_kick) return;

    if (g_state == State::SECURING) {
        g_kick = false;
        // A beat after connect: let MTU exchange and the app's own discovery
        // settle before the pairing dialog jumps the queue.
        if (!g_secureAskedMs) {
            g_secureAskedMs = millis();
            g_kick = true;
            return;
        }
        if (millis() - g_secureAskedMs < 1500) { g_kick = true; return; }
        NimBLEConnInfo info =
            NimBLEDevice::getServer()->getPeerInfoByHandle(g_conn);
        if (info.isEncrypted()) {
            g_state = State::DISCOVER;   // bonded already: no dialog needed
        } else {
            diag::log("ams: requesting encryption");
            NimBLEDevice::startSecurity(g_conn);
            return;   // onSecured moves the state on
        }
    }

    if (g_state == State::DISCOVER) {
        g_kick = false;
        NimBLEClient* client = NimBLEDevice::getServer()->getClient(g_conn);
        if (!client) { g_state = State::UNAVAILABLE; return; }
        NimBLERemoteService* svc = client->getService(AMS_SVC);
        if (!svc) {
            g_state = State::UNAVAILABLE;
            diag::log("ams: phone doesn't expose AMS on this link");
            return;
        }
        g_remoteCmd = svc->getCharacteristic(AMS_REMOTE_CMD);
        NimBLERemoteCharacteristic* upd = svc->getCharacteristic(AMS_ENTITY_UPD);
        if (!upd || !upd->subscribe(true, onEntityUpdate)) {
            g_state = State::UNAVAILABLE;
            diag::log("ams: entity-update subscribe failed");
            return;
        }
        // Tell AMS which attributes to stream. One write per entity.
        uint8_t track[] = {ENTITY_TRACK, TRACK_ATTR_ARTIST, TRACK_ATTR_ALBUM,
                           TRACK_ATTR_TITLE, TRACK_ATTR_DURATION};
        uint8_t player[] = {ENTITY_PLAYER, PLAYER_ATTR_PLAYBACK_INFO};
        bool ok = upd->writeValue(track, sizeof(track), true) &&
                  upd->writeValue(player, sizeof(player), true);
        if (!ok) {
            g_state = State::UNAVAILABLE;
            diag::log("ams: entity subscription writes failed");
            return;
        }
        g_state = State::RUNNING;
        diag::log("ams: up — system-wide now-playing via the phone");
    }
}

bool sendCommand(uint8_t mediaCmd) {
    if (g_state != State::RUNNING || !g_remoteCmd) return false;
    uint8_t id;
    switch (mediaCmd) {
        case MC_TOGGLE: id = CMD_TOGGLE; break;
        case MC_NEXT: id = CMD_NEXT; break;
        case MC_PREV: id = CMD_PREV; break;
        case MC_VOL_UP: id = CMD_VOL_UP; break;
        case MC_VOL_DOWN: id = CMD_VOL_DOWN; break;
        default: return false;
    }
    return g_remoteCmd->writeValue(&id, 1, true);
}

}  // namespace ams
