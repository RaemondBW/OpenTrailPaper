#include "mesh_service.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstring>
#include <ctime>

#include "config.h"
#include "diag.h"
#include "lora_radio.h"
#include "settings.h"

namespace {

using mesh::BROADCAST_ADDR;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

SemaphoreHandle_t lock = nullptr;
inline void take() { if (lock) xSemaphoreTake(lock, portMAX_DELAY); }
inline void give() { if (lock) xSemaphoreGive(lock); }

// The message ring lives in PSRAM. 48 messages is ~11 KB, and internal DRAM on
// this board is the scarce resource — boot finishes with ~124 KB spare and the
// panel and both BLE stacks are already in it (see the ordering note in
// main.cpp). Only the mesh task and the BLE server task read this, never an ISR,
// so PSRAM is safe here.
mesh_service::Message* msgs = nullptr;
int msgCount = 0;         // entries in use, up to MESH_MSG_HISTORY
int msgHead = 0;          // where the next message goes (ring)
int unread = 0;

mesh_service::Node nodes[MESH_NODE_MAX];
int nodeUsed = 0;

mesh_service::Stats st;

bool changed = false;
bool radioUp = false;
bool meshEnabled = true;
uint32_t myNodeNum = 0;
char myLongName[40] = {};
char myShortName[8] = {};

// Channels. Slot 0 is PRIMARY: its name sets the frequency slot, so it is the one
// that decides which mesh this device is on. Slots 1.. are private channels,
// riding the same RF and told apart by the header's channel-hash byte plus their
// own key. Two people can therefore share a private channel only if their
// primaries agree — otherwise they are not even on the same frequency.
struct Chan {
    bool     used = false;
    char     name[16] = {};
    uint8_t  psk[mesh::MAX_PSK_LEN] = {};   // as stored/shared: 0, 1, 16 or 32 bytes
    size_t   pskLen = 0;
    uint8_t  key[mesh::MAX_PSK_LEN] = {};   // the expansion the cipher uses
    size_t   keyLen = 0;
    uint8_t  hash = 0;
};
Chan chans[mesh::MAX_CHANNELS];

// The channel name the rider explicitly chose, or "" for "follow the modem".
//
// Empty is the normal state and is what makes this interoperate. On a stock
// Meshtastic node the primary channel has NO name, and both the frequency slot and
// the channel byte are derived from the modem preset's name instead
// (Channels::getName()) — so changing preset there moves the channel too, and a
// node on the MediumFast preset sits on MediumFast's slot, not LongFast's. Pinning
// a name here would put us on a channel no stock node uses the moment the preset
// changed. An explicit name overrides, which is the private-channel case, and then
// the preset really is independent.
char chanExplicit[16] = {};
uint8_t chanPskIndex = 1;
uint8_t chanPsk[16] = {};
uint8_t chanHash = 0;
// Which modem we speak it with. Separate setting, separate meaning — see the
// note in config.h. Bandwidth comes from here too, so the preset also moves the
// frequency whenever it changes the bandwidth.
uint8_t presetIdx = MESH_PRESET_DEFAULT;

// Recently seen (sender, id) pairs. The mesh floods, so the same packet arrives
// more than once whenever anyone rebroadcasts it; without this the phone shows
// every message two or three times.
constexpr int SEEN_MAX = 64;
struct Seen { uint32_t sender, id; };
Seen seen[SEEN_MAX];
int seenHead = 0;

// Outbox. Frames are built (encoded and encrypted) at queue time so the task
// only has to find a clear channel and hand them to the radio.
constexpr int OUTBOX_MAX = 6;
struct Outgoing {
    uint8_t  frame[mesh::MAX_RADIO_LEN];
    size_t   len = 0;
    uint32_t id = 0;
    bool     wantAck = false;
    uint8_t  attempts = 0;
};
Outgoing outbox[OUTBOX_MAX];
int outboxCount = 0;

// Set by the task while a frame is on the air.
uint32_t txStartMs = 0;
uint32_t txTimeoutMs = 0;
uint32_t lastTxEndMs = 0;

// Wakes the task from its idle block: the radio ISR gives this, and so does
// queueText() so a message the rider just sent from the phone goes out now
// rather than at the next tick.
SemaphoreHandle_t wake = nullptr;

// Config changes asked for by another task, applied by the mesh task.
//
// LOAD-BEARING. Every one of these restarts or re-tunes the radio, and
// lora_radio has no internal locking — by its own contract one task owns it.
// These setters are called from the BLE server task (the phone's Mesh screen)
// and from the UI task (the `mesh` console command), so applying them inline
// would have a second task calling begin() while this one is part-way through a
// transmission, which spans startSend / irq / finishSend and cannot survive it.
// So they are staged here and picked up at the top of the task loop.
volatile int8_t reqPreset = -1;        // -1 = nothing, else a preset index
volatile int8_t reqEnable = -1;        // -1 = nothing, 0 = off, 1 = on
char reqChanName[16] = {};
volatile uint8_t reqChanKey = 0;
volatile bool reqChan = false;
// Private-channel edits, staged the same way. pskLen 0xFF means "delete".
volatile int8_t reqPrivIdx = -1;
char reqPrivName[16] = {};
uint8_t reqPrivPsk[mesh::MAX_PSK_LEN] = {};
volatile uint8_t reqPrivPskLen = 0;

// Politeness gap between our own transmissions. The US band has no duty-cycle
// ceiling, but a node that answers instantly and repeatedly is the one that
// collides with everybody; Meshtastic spaces its own traffic for the same reason.
constexpr uint32_t MIN_TX_GAP_MS = 1500;

// How long after boot to announce ourselves, then how often to repeat it. Six
// hours rather than Meshtastic's 3 h default: the Bay Area Mesh asks for "6 hour
// +" on automatic beacons, and this is the only thing this firmware puts on the
// air unprompted, so it is the only interval there is to be a good citizen about.
// Still often enough that a neighbour learns our name within a ride.
//   https://bayme.sh/docs/getting-started/recommended-settings/
constexpr uint32_t NODEINFO_FIRST_MS = 20000;
constexpr uint32_t NODEINFO_EVERY_MS = 6UL * 60UL * 60UL * 1000UL;
uint32_t nextNodeInfoMs = NODEINFO_FIRST_MS;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

uint32_t nowUtc() {
    const time_t t = time(nullptr);
    // Before GPS or the RTC has set the clock this is 1970, which would be worse
    // than admitting we do not know: the phone stamps its own receive time then.
    return t > 1735689600 ? (uint32_t)t : 0;
}

// Meshtastic derives a node number from the last four bytes of the board's MAC,
// which is what makes an id stable across reboots and unique on a mesh. Copied
// the same way so this device answers to the same "!xxxxxxxx" a stock Meshtastic
// build on the same hardware would.
uint32_t deriveNodeNum() {
    uint8_t mac[8] = {};
    if (esp_efuse_mac_get_default(mac) != ESP_OK) return 0;
    uint32_t n = 0;
    memcpy(&n, mac + 2, sizeof(n));
    return n;
}

// The name that actually goes into both hashes.
const char* effectiveChan() {
    return chanExplicit[0] ? chanExplicit : mesh::preset(presetIdx).name;
}

// Recomputes one channel's key and hash from its stored name and psk.
void deriveChan(Chan& c, const char* name) {
    c.keyLen = mesh::expandPsk(c.psk, c.pskLen, c.key);
    // The hash is over the STORED psk, not the expansion — a single-byte
    // well-known key hashes as that byte, which is what stock nodes do.
    c.hash = mesh::channelHash(name, c.psk, c.pskLen);
}

// The private channels, stored as a ChannelSet — the same encoding a share URL
// uses, so there is one representation of a channel rather than two that can
// disagree. Slot 0 is not in here: it is derived from the preset and the channel
// name, which have their own settings.
void savePrivateChannels() {
    mesh::ChannelSettings set[mesh::MAX_CHANNELS];
    int n = 0;
    for (int i = 1; i < mesh::MAX_CHANNELS; ++i) {
        if (!chans[i].used) continue;
        snprintf(set[n].name, sizeof(set[n].name), "%s", chans[i].name);
        memcpy(set[n].psk, chans[i].psk, chans[i].pskLen);
        set[n].pskLen = chans[i].pskLen;
        // The slot is implied by order, so a deleted middle channel renumbers the
        // ones after it. That is fine: the phone re-reads the table after any edit.
        n++;
    }
    uint8_t buf[mesh::MAX_CHANNELS * 80];
    const size_t len = n ? mesh::encodeChannelSet(set, n, buf, sizeof(buf)) : 0;
    settings::setMeshPrivateChannels(buf, len);
}

void loadPrivateChannels() {
    uint8_t buf[mesh::MAX_CHANNELS * 80];
    const size_t len = settings::meshPrivateChannels(buf, sizeof(buf));
    if (!len) return;
    mesh::ChannelSettings set[mesh::MAX_CHANNELS];
    int n = 0;
    if (!mesh::decodeChannelSet(buf, len, set, mesh::MAX_CHANNELS, n)) {
        diag::log("mesh: stored channel set is corrupt — ignoring it");
        return;
    }
    for (int i = 0; i < n && i + 1 < mesh::MAX_CHANNELS; ++i) {
        Chan& c = chans[i + 1];
        c = Chan();
        c.used = true;
        snprintf(c.name, sizeof(c.name), "%s", set[i].name);
        memcpy(c.psk, set[i].psk, set[i].pskLen);
        c.pskLen = set[i].pskLen;
    }
}

void applyChannel() {
    // Slot 0 tracks the primary channel's name (which may come from the modem)
    // and the well-known key index chosen for it.
    Chan& p = chans[0];
    p.used = true;
    snprintf(p.name, sizeof(p.name), "%s", effectiveChan());
    p.psk[0] = chanPskIndex;
    p.pskLen = 1;
    deriveChan(p, p.name);

    // Kept for everything that still asks the primary directly.
    memcpy(chanPsk, p.key, sizeof(chanPsk));
    chanHash = p.hash;

    for (int i = 1; i < mesh::MAX_CHANNELS; ++i)
        if (chans[i].used) deriveChan(chans[i], chans[i].name);
}

// Which channel a received packet belongs to, by its header hash. -1 if none of
// ours. Slot 0 is checked first so the primary always wins a collision: an 8-bit
// hash over a handful of channels will collide eventually, and the primary is the
// one whose traffic must not be misattributed.
int chanForHash(uint8_t h) {
    for (int i = 0; i < mesh::MAX_CHANNELS; ++i)
        if (chans[i].used && chans[i].hash == h) return i;
    return -1;
}

float channelFreq() {
    return mesh::channelFrequencyMHz(effectiveChan(), mesh::preset(presetIdx).bwKhz,
                                     MESH_FREQ_START_MHZ, MESH_FREQ_END_MHZ,
                                     MESH_SPACING_MHZ);
}

bool startRadio() {
    const mesh::ModemPreset& p = mesh::preset(presetIdx);
    lora_radio::Config c;
    c.freqMHz = channelFreq();
    c.bwKhz = p.bwKhz;
    c.sf = p.sf;
    c.cr = p.cr;
    c.syncWord = MESH_SYNC_WORD;
    c.preambleLen = MESH_PREAMBLE_LEN;
    c.powerDbm = MESH_TX_DBM;
    return lora_radio::begin(c);
}

// A packet id must be non-zero and unpredictable — it is half the encryption
// nonce, so a repeat under the same key would leak the XOR of two messages.
uint32_t newPacketId() {
    uint32_t id = 0;
    while (id == 0) id = esp_random();
    return id;
}

bool alreadySeen(uint32_t sender, uint32_t id) {
    for (int i = 0; i < SEEN_MAX; ++i)
        if (seen[i].sender == sender && seen[i].id == id) return true;
    seen[seenHead] = {sender, id};
    seenHead = (seenHead + 1) % SEEN_MAX;
    return false;
}

// Caller holds the lock.
mesh_service::Message* pushMessage() {
    mesh_service::Message* m = &msgs[msgHead];
    *m = mesh_service::Message();
    msgHead = (msgHead + 1) % MESH_MSG_HISTORY;
    if (msgCount < MESH_MSG_HISTORY) msgCount++;
    changed = true;
    return m;
}

// Caller holds the lock. Returns the entry for `num`, evicting the node we have
// not heard from in longest if the table is full.
mesh_service::Node* nodeFor(uint32_t num) {
    for (int i = 0; i < nodeUsed; ++i)
        if (nodes[i].num == num) return &nodes[i];
    if (nodeUsed < MESH_NODE_MAX) {
        mesh_service::Node* n = &nodes[nodeUsed++];
        *n = mesh_service::Node();
        n->num = num;
        return n;
    }
    int oldest = 0;
    for (int i = 1; i < nodeUsed; ++i)
        if (nodes[i].lastHeardMs < nodes[oldest].lastHeardMs) oldest = i;
    nodes[oldest] = mesh_service::Node();
    nodes[oldest].num = num;
    return &nodes[oldest];
}

// Builds a frame and puts it in the outbox. Caller holds the lock.
uint32_t enqueue(uint32_t dest, uint8_t portnum, const uint8_t* payload,
                 size_t payloadLen, bool wantAck, uint32_t requestId,
                 uint8_t channel) {
    if (outboxCount >= OUTBOX_MAX) return 0;
    if (channel >= mesh::MAX_CHANNELS || !chans[channel].used) return 0;

    mesh::Data d;
    d.portnum = portnum;
    if (payloadLen > sizeof(d.payload)) return 0;
    memcpy(d.payload, payload, payloadLen);
    d.payloadLen = payloadLen;
    d.requestId = requestId;

    Outgoing& o = outbox[outboxCount];
    o.id = newPacketId();
    o.wantAck = wantAck;
    o.attempts = 0;

    mesh::Header h;
    h.dest = dest;
    h.sender = myNodeNum;
    h.id = o.id;
    h.hopLimit = MESH_HOP_LIMIT;
    h.hopStart = MESH_HOP_LIMIT;
    h.wantAck = wantAck;
    h.channelHash = chans[channel].hash;
    mesh::encodeHeader(h, o.frame);

    const size_t n = mesh::encodeData(d, o.frame + mesh::HEADER_LEN,
                                      sizeof(o.frame) - mesh::HEADER_LEN);
    if (n == 0) return 0;
    // Encrypted here rather than at send time: the nonce is (sender, packet id),
    // both already fixed, so there is nothing to gain by waiting — and a retry
    // must send the identical bytes.
    mesh::ctrCrypt(chans[channel].key, chans[channel].keyLen, myNodeNum, o.id, 0,
                   o.frame + mesh::HEADER_LEN, n);
    o.len = mesh::HEADER_LEN + n;

    outboxCount++;
    return o.id;
}

// Caller holds the lock.
void queueNodeInfo(uint32_t dest, uint8_t channel) {
    mesh::User u;
    mesh::nodeIdString(myNodeNum, u.id, sizeof(u.id));
    snprintf(u.longName, sizeof(u.longName), "%s", myLongName);
    snprintf(u.shortName, sizeof(u.shortName), "%s", myShortName);
    // hw_model stays UNSET: this board is not in Meshtastic's HardwareModel enum,
    // and claiming a model we are not would mislead anything that acts on it.
    uint8_t buf[128];
    const size_t n = mesh::encodeUser(u, buf, sizeof(buf));
    if (n) enqueue(dest, mesh::PORT_NODEINFO, buf, n, false, 0, channel);
}

// Caller holds the lock.
void queueAck(uint32_t dest, uint32_t requestId, uint8_t channel) {
    uint8_t buf[8];
    const size_t n = mesh::encodeRouting(0, buf, sizeof(buf));   // NONE = ACK
    if (n) enqueue(dest, mesh::PORT_ROUTING, buf, n, false, requestId, channel);
}

// ---------------------------------------------------------------------------
// Receive
// ---------------------------------------------------------------------------

void handleFrame(const uint8_t* frame, size_t len, float rssi, float snr) {
    mesh::Header h;
    if (!mesh::decodeHeader(frame, len, h)) {
        st.rxDropped++;
        return;
    }
    if (h.sender == myNodeNum) return;          // our own packet, echoed back
    const int ch = chanForHash(h.channelHash);
    if (ch < 0) {
        // A channel we do not hold (or an unencrypted packet, which stamps 0).
        // Without the key it is noise, and it is not ours to relay.
        st.rxOtherChannel++;
        return;
    }
    if (alreadySeen(h.sender, h.id)) {
        st.rxDuplicate++;
        return;
    }

    uint8_t payload[mesh::MAX_PAYLOAD_LEN];
    const size_t plen = len - mesh::HEADER_LEN;
    if (plen == 0 || plen > sizeof(payload)) {
        st.rxDropped++;
        return;
    }
    memcpy(payload, frame + mesh::HEADER_LEN, plen);
    mesh::ctrCrypt(chans[ch].key, chans[ch].keyLen, h.sender, h.id, 0, payload,
                   plen);

    mesh::Data d;
    if (!mesh::decodeData(payload, plen, d)) {
        // Right channel hash but undecodable. Almost always a directly-addressed
        // packet encrypted to a public key we do not have (Meshtastic 2.5+ does
        // that when it knows a peer's key). We publish no key, so peers fall back
        // to the channel key for us — but a packet meant for a third party still
        // lands here.
        st.rxDropped++;
        return;
    }
    st.rx++;

    const uint8_t hops = h.hopStart >= h.hopLimit
                             ? (uint8_t)(h.hopStart - h.hopLimit) : 0;
    const uint32_t utc = nowUtc();

    take();
    mesh_service::Node* n = nodeFor(h.sender);
    n->lastHeardMs = millis();
    n->lastHeardUtc = utc;
    n->snr = (int8_t)snr;
    n->rssi = (int8_t)rssi;
    n->hops = hops;

    const bool forUs = (h.dest == myNodeNum);
    const bool forEveryone = (h.dest == BROADCAST_ADDR);

    switch (d.portnum) {
    case mesh::PORT_TEXT: {
        if (!forUs && !forEveryone) break;       // somebody else's conversation
        mesh_service::Message* m = pushMessage();
        m->id = h.id;
        m->from = h.sender;
        m->to = h.dest;
        m->utc = utc;
        m->ms = millis();
        m->rssi = (int8_t)rssi;
        m->snr = (int8_t)snr;
        m->hops = hops;
        m->outgoing = false;
        m->channel = (uint8_t)ch;
        size_t tn = d.payloadLen;
        if (tn > mesh::MAX_TEXT_LEN) tn = mesh::MAX_TEXT_LEN;
        memcpy(m->text, d.payload, tn);
        m->text[tn] = 0;
        unread++;
        // Only a direct message is acknowledged. Acking a broadcast would put
        // one packet on the air per node that heard it.
        if (forUs && h.wantAck) queueAck(h.sender, h.id, (uint8_t)ch);
        diag::log("mesh: msg from !%08x on '%s' (%d dBm, %d hop%s): %.60s",
                  (unsigned)h.sender, chans[ch].name, (int)rssi, hops,
                  hops == 1 ? "" : "s", m->text);
        break;
    }
    case mesh::PORT_NODEINFO: {
        mesh::User u;
        if (mesh::decodeUser(d.payload, d.payloadLen, u)) {
            if (u.longName[0])
                snprintf(n->longName, sizeof(n->longName), "%s", u.longName);
            if (u.shortName[0])
                snprintf(n->shortName, sizeof(n->shortName), "%s", u.shortName);
            changed = true;
        }
        // A node asking who is out there gets an answer, which is how our name
        // reaches a phone that just joined the mesh.
        if (d.wantResponse) queueNodeInfo(h.sender, (uint8_t)ch);
        break;
    }
    case mesh::PORT_ROUTING: {
        uint32_t err = 0;
        if (!forUs) break;
        if (mesh::decodeRoutingError(d.payload, d.payloadLen, err)) {
            for (int i = 0; i < msgCount; ++i) {
                mesh_service::Message* m = &msgs[i];
                if (m->outgoing && m->id == d.requestId) {
                    m->status = err == 0 ? mesh_service::TX_ACKED
                                         : mesh_service::TX_FAILED;
                    changed = true;
                    if (err == 0) st.acksRx++;
                    break;
                }
            }
        }
        break;
    }
    case mesh::PORT_POSITION: {
        // Decoded but never sent: we show where other people are, and do not tell
        // the mesh where we are (docs/meshtastic.md).
        mesh::Position pos;
        if (mesh::decodePosition(d.payload, d.payloadLen, pos) && pos.valid) {
            n->hasPosition = true;
            n->latitude = pos.latitude;
            n->longitude = pos.longitude;
            n->altitudeM = pos.altitudeM;
            n->satsInView = pos.satsInView;
            n->precisionBits = pos.precisionBits;
            n->positionMs = millis();
            changed = true;
        }
        break;
    }
    default:
        // Some other app (telemetry, neighbour info, …). The node is now known to
        // be out there and that is all we take from it.
        break;
    }
    give();
}

// ---------------------------------------------------------------------------
// Transmit
// ---------------------------------------------------------------------------

// Drops the frame at the head of the outbox and records `status` against the
// message it belonged to, so the phone's tick/cross reflects what happened. The
// only way a frame leaves the outbox — every exit needs both halves done, and
// doing them separately is how one of them gets forgotten. Takes the lock itself.
void popOutbox(uint8_t status) {
    take();
    if (outboxCount > 0) {
        const uint32_t id = outbox[0].id;
        for (int i = 1; i < outboxCount; ++i) outbox[i - 1] = outbox[i];
        outboxCount--;
        if (status == mesh_service::TX_SENT) st.tx++;
        else st.txFailed++;
        for (int i = 0; i < msgCount; ++i) {
            // Only PENDING is overwritten: an ack can land before finishSend()
            // returns, and ACKED must not be walked back to SENT.
            if (msgs[i].outgoing && msgs[i].id == id &&
                msgs[i].status == mesh_service::TX_PENDING) {
                msgs[i].status = status;
                changed = true;
                break;
            }
        }
    }
    give();
}

void serviceOutbox() {
    if (lora_radio::transmitting()) {
        if (lora_radio::irqFired()) {
            lora_radio::finishSend();
            lastTxEndMs = millis();
            popOutbox(mesh_service::TX_SENT);
        } else if (millis() - txStartMs > txTimeoutMs) {
            lora_radio::abortSend();
            lastTxEndMs = millis();
            popOutbox(mesh_service::TX_FAILED);
        }
        return;
    }

    take();
    const bool have = outboxCount > 0;
    give();
    if (!have) return;
    if (millis() - lastTxEndMs < MIN_TX_GAP_MS) return;

    // Listen before transmitting. A LongFast packet occupies the channel for a
    // second or more, so talking over one wastes both.
    if (lora_radio::channelBusy()) {
        take();
        // Count the miss, but do NOT give up on the message. After enough tries
        // we transmit over whatever is there instead of dropping it: a rider who
        // typed "flat tire, going back" would rather have it collide and be
        // resent than silently vanish, and a channel-activity scan that reports
        // busy forever is at least as likely to be a bad CAD threshold at SF11 as
        // a genuinely saturated band.
        const bool sendAnyway = outboxCount > 0 && ++outbox[0].attempts >= 8;
        give();
        if (!sendAnyway) {
            // Randomised backoff: a fixed one would have two nodes that collided
            // collide again on the retry.
            vTaskDelay(pdMS_TO_TICKS(120 + (esp_random() % 400)));
            return;
        }
        diag::log("mesh: channel still busy after 8 tries — sending anyway");
    }

    take();
    uint8_t frame[mesh::MAX_RADIO_LEN];
    size_t len = 0;
    if (outboxCount > 0) {
        len = outbox[0].len;
        memcpy(frame, outbox[0].frame, len);
    }
    give();
    if (!len) return;

    if (lora_radio::startSend(frame, len)) {
        txStartMs = millis();
        // Generous: twice the calculated air time plus a floor, so a slow module
        // is never mistaken for a stuck one.
        txTimeoutMs = lora_radio::airtimeMs(len) * 2 + 1000;
    } else {
        popOutbox(mesh_service::TX_FAILED);
    }
}

}  // namespace

namespace mesh_service {

bool begin() {
    lock = xSemaphoreCreateMutex();
    wake = xSemaphoreCreateBinary();

    msgs = (Message*)heap_caps_calloc(MESH_MSG_HISTORY, sizeof(Message),
                                      MALLOC_CAP_SPIRAM);
    if (!msgs) {
        // Without the ring there is nowhere to put a message, so the feature is
        // off rather than half-working.
        diag::log("mesh: no PSRAM for the message ring — messaging disabled");
        return false;
    }

    myNodeNum = deriveNodeNum();
    meshEnabled = settings::meshEnabled();
    chanPskIndex = settings::meshChannelKey();
    presetIdx = settings::meshPreset();
    snprintf(chanExplicit, sizeof(chanExplicit), "%s", settings::meshChannel());
    snprintf(myLongName, sizeof(myLongName), "%s", settings::meshLongName());
    snprintf(myShortName, sizeof(myShortName), "%s", settings::meshShortName());
    if (!myLongName[0]) {
        // Default identity: something a neighbour can read, ending in the node
        // id so two of these on the same ride are distinguishable.
        snprintf(myLongName, sizeof(myLongName), "OpenTrail %04x",
                 (unsigned)(myNodeNum & 0xFFFF));
    }
    if (!myShortName[0])
        snprintf(myShortName, sizeof(myShortName), "%03x",
                 (unsigned)(myNodeNum & 0xFFF));
    loadPrivateChannels();
    applyChannel();

    char id[16];
    mesh::nodeIdString(myNodeNum, id, sizeof(id));
    diag::log("mesh: node %s '%s' (%s) channel '%s' hash 0x%02x, modem %s "
              "(SF%u BW%.0f)", id, myLongName, myShortName, effectiveChan(), chanHash,
              mesh::preset(presetIdx).name, mesh::preset(presetIdx).sf,
              mesh::preset(presetIdx).bwKhz);

    if (!meshEnabled) {
        diag::log("mesh: disabled in settings — radio left off");
        return true;      // the service is fine; the radio is simply not wanted
    }
    radioUp = startRadio();
    if (radioUp)
        diag::log("mesh: %s region, %.4f MHz", MESH_REGION_NAME, channelFreq());
    return true;
}

void applyEnabled(bool on);
void applyPreset(uint8_t index);
void applyPrivateChannel(uint8_t index, const char* name, const uint8_t* psk,
                         size_t pskLen, bool remove);
void applyChannelChange(const char* name, uint8_t pskIndex);

// Anything another task asked for, applied here where the radio is owned. Runs
// before the enabled/radioUp check below, so a "turn it on" request from a device
// that booted with mesh off still gets serviced.
void applyPendingConfig() {
    if (reqEnable >= 0) {
        const bool on = reqEnable != 0;
        reqEnable = -1;
        applyEnabled(on);
    }
    if (reqChan) {
        char name[sizeof(reqChanName)];
        take();
        snprintf(name, sizeof(name), "%s", reqChanName);
        const uint8_t key = reqChanKey;
        reqChan = false;
        give();
        applyChannelChange(name, key);
    }
    if (reqPreset >= 0) {
        const uint8_t idx = (uint8_t)reqPreset;
        reqPreset = -1;
        applyPreset(idx);
    }
    if (reqPrivIdx >= 0) {
        char name[sizeof(reqPrivName)];
        uint8_t psk[mesh::MAX_PSK_LEN];
        take();
        snprintf(name, sizeof(name), "%s", reqPrivName);
        memcpy(psk, reqPrivPsk, sizeof(psk));
        const uint8_t len = reqPrivPskLen;
        const uint8_t idx = (uint8_t)reqPrivIdx;
        reqPrivIdx = -1;
        give();
        applyPrivateChannel(idx, name, psk, len == 0xFF ? 0 : len, len == 0xFF);
    }
}

void task(void*) {
    uint8_t frame[mesh::MAX_RADIO_LEN];
    for (;;) {
        // Blocks until the radio interrupt (or a queued message) wakes us, with a
        // 250 ms backstop for the outbox's own timers. Nothing here spins.
        if (wake) xSemaphoreTake(wake, pdMS_TO_TICKS(250));

        applyPendingConfig();

        if (!radioUp || !meshEnabled) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!lora_radio::transmitting() && lora_radio::irqFired()) {
            float rssi = 0, snr = 0;
            const size_t n = lora_radio::read(frame, sizeof(frame), rssi, snr);
            if (n >= mesh::HEADER_LEN) {
                handleFrame(frame, n, rssi, snr);
            } else if (n == 0) {
                st.rxDropped++;      // CRC failure or a runt
            }
        }

        serviceOutbox();

        // Announce ourselves so neighbours can put a name to our messages.
        if (millis() >= nextNodeInfoMs) {
            nextNodeInfoMs = millis() + NODEINFO_EVERY_MS;
            take();
            queueNodeInfo(BROADCAST_ADDR, 0);
            give();
        }
    }
}

bool radioOk() { return radioUp; }
bool enabled() { return meshEnabled; }

// Applies a staged enable/disable. Mesh task only.
void applyEnabled(bool on) {
    if (on == meshEnabled) return;
    meshEnabled = on;
    settings::setMeshEnabled(on);
    if (on) {
        if (!radioUp) radioUp = startRadio();
        else lora_radio::listen();
        // Re-announce shortly after coming back, so neighbours re-learn us.
        nextNodeInfoMs = millis() + 5000;
    } else {
        // Idle receive is a few milliamps that a rider who turned this off is not
        // expecting to pay. The module keeps its config, so coming back is a
        // startReceive() rather than a full re-init.
        lora_radio::sleep();
        take();
        outboxCount = 0;      // nothing queued is going anywhere now
        give();
    }
    diag::log("mesh: %s", on ? "enabled" : "disabled");
}

uint32_t nodeNum() { return myNodeNum; }
float frequencyMHz() { return channelFreq(); }
const char* channelName() { return effectiveChan(); }
bool channelFollowsPreset() { return chanExplicit[0] == 0; }
const char* longName() { return myLongName; }
const char* shortName() { return myShortName; }
uint8_t channelPskIndex() { return chanPskIndex; }
uint8_t presetIndex() { return presetIdx; }

int channelCount() {
    int n = 0;
    take();
    for (int i = 0; i < mesh::MAX_CHANNELS; ++i) if (chans[i].used) n++;
    give();
    return n;
}

bool channelAt(int index, ChannelInfo& out) {
    if (index < 0 || index >= mesh::MAX_CHANNELS) return false;
    take();
    const bool ok = chans[index].used;
    if (ok) {
        out.index = (uint8_t)index;
        out.used = true;
        snprintf(out.name, sizeof(out.name), "%s", chans[index].name);
        memcpy(out.psk, chans[index].psk, chans[index].pskLen);
        out.pskLen = (uint8_t)chans[index].pskLen;
        out.hash = chans[index].hash;
    }
    give();
    return ok;
}

int firstFreeChannel() {
    take();
    int free = -1;
    for (int i = 1; i < mesh::MAX_CHANNELS; ++i) {
        if (!chans[i].used) { free = i; break; }
    }
    give();
    return free;
}

// Applies a staged private-channel edit. Mesh task only.
void applyPrivateChannel(uint8_t index, const char* name, const uint8_t* psk,
                         size_t pskLen, bool remove) {
    if (index == 0 || index >= mesh::MAX_CHANNELS) return;
    take();
    if (remove) {
        // Drop the messages that arrived on it too — leaving them readable after
        // the key is gone would be a surprising kind of "deleted".
        for (int i = 0; i < msgCount; ++i) {
            if (msgs[i].channel == index) msgs[i] = Message();
        }
        chans[index] = Chan();
        diag::log("mesh: forgot private channel %u", index);
    } else {
        Chan& c = chans[index];
        c = Chan();
        c.used = true;
        snprintf(c.name, sizeof(c.name), "%s", name);
        if (pskLen > mesh::MAX_PSK_LEN) pskLen = mesh::MAX_PSK_LEN;
        memcpy(c.psk, psk, pskLen);
        c.pskLen = pskLen;
        deriveChan(c, c.name);
        diag::log("mesh: private channel %u '%s' hash 0x%02x, %u-byte key", index,
                  c.name, c.hash, (unsigned)c.pskLen);
    }
    changed = true;
    give();
    savePrivateChannels();
}

bool setPrivateChannel(uint8_t index, const char* name, const uint8_t* psk,
                       size_t pskLen) {
    // Slot 0 is the primary and decides the frequency; it is changed through
    // setChannel()/setPreset(), never here.
    if (index == 0 || index >= mesh::MAX_CHANNELS || !name) return false;
    if (pskLen > mesh::MAX_PSK_LEN) return false;
    take();
    snprintf(reqPrivName, sizeof(reqPrivName), "%s", name);
    memcpy(reqPrivPsk, psk, pskLen);
    reqPrivPskLen = (uint8_t)pskLen;
    reqPrivIdx = (int8_t)index;
    give();
    if (wake) xSemaphoreGive(wake);
    return true;
}

bool deletePrivateChannel(uint8_t index) {
    if (index == 0 || index >= mesh::MAX_CHANNELS) return false;
    take();
    reqPrivPskLen = 0xFF;          // the delete marker
    reqPrivIdx = (int8_t)index;
    give();
    if (wake) xSemaphoreGive(wake);
    return true;
}

// The three setters below only STAGE the change; the mesh task applies it within
// a tick. Callers are the BLE server task and the console, neither of which may
// touch the radio — see the note on reqPreset above. The getters keep returning
// the old value until it lands, which is why the phone is sent a fresh state
// snapshot after the change rather than trusting its own optimism.
void setEnabled(bool on) {
    reqEnable = on ? 1 : 0;
    if (wake) xSemaphoreGive(wake);
}

void setPreset(uint8_t index) {
    if (index >= mesh::PRESET_COUNT) return;
    reqPreset = (int8_t)index;
    if (wake) xSemaphoreGive(wake);
}

void setChannel(const char* name, uint8_t pskIndex) {
    // Only a null pointer is refused. An EMPTY name is the meaningful request to
    // stop pinning a channel and follow the modem preset again — rejecting it here
    // (while the applier accepted it) is why "mesh channel default" silently did
    // nothing.
    if (!name) return;
    take();
    snprintf(reqChanName, sizeof(reqChanName), "%s", name);
    reqChanKey = pskIndex;
    reqChan = true;
    give();
    if (wake) xSemaphoreGive(wake);
}
const char* presetName() { return mesh::preset(presetIdx).name; }

// Applies a staged preset change. Mesh task only.
void applyPreset(uint8_t index) {
    if (index >= mesh::PRESET_COUNT || index == presetIdx) return;
    // With no explicit channel name the preset IS the channel name, so switching
    // preset moves the frequency slot and the channel byte as well as the speed —
    // exactly as it does on a stock node. Remember whether that happened.
    const bool wasFollowing = channelFollowsPreset();
    presetIdx = index;
    settings::setMeshPreset(index);
    const mesh::ModemPreset& p = mesh::preset(presetIdx);
    applyChannel();          // re-derive the hash: the effective name may have moved
    take();
    // Anything queued was encrypted for a frame we are about to stop sending.
    outboxCount = 0;
    if (wasFollowing) {
        // A different channel, not merely a different speed: the neighbours and
        // messages belong to the one we just left.
        msgCount = msgHead = unread = 0;
        nodeUsed = 0;
        memset(seen, 0, sizeof(seen));
    }
    changed = true;
    give();
    diag::log("mesh: modem %s (SF%u BW%.0f CR4/%u) -> channel '%s' hash 0x%02x "
              "%.4f MHz", p.name, p.sf, p.bwKhz, p.cr, effectiveChan(), chanHash,
              channelFreq());
    if (meshEnabled) {
        // Restarted rather than tweaked: bandwidth is part of a preset, and a
        // bandwidth change moves the frequency slot as well as the modem.
        radioUp = startRadio();
        nextNodeInfoMs = millis() + 5000;
    }
}

void setNames(const char* longName, const char* shortName) {
    if (longName && longName[0])
        snprintf(myLongName, sizeof(myLongName), "%s", longName);
    if (shortName && shortName[0])
        snprintf(myShortName, sizeof(myShortName), "%s", shortName);
    settings::setMeshNames(myLongName, myShortName);
    diag::log("mesh: renamed to '%s' (%s)", myLongName, myShortName);
    if (radioUp && meshEnabled) {
        take();
        queueNodeInfo(BROADCAST_ADDR, 0);
        give();
        if (wake) xSemaphoreGive(wake);
    }
}

// Applies a staged channel change. Mesh task only.
void applyChannelChange(const char* name, uint8_t pskIndex) {
    if (!name) return;
    if (pskIndex < 1) pskIndex = 1;
    if (pskIndex > 10) pskIndex = 10;
    // An empty name means "follow the modem preset" — the interoperable default,
    // not a rejected input.
    if (strcmp(name, chanExplicit) == 0 && pskIndex == chanPskIndex) return;

    snprintf(chanExplicit, sizeof(chanExplicit), "%s", name);
    chanPskIndex = pskIndex;
    applyChannel();
    settings::setMeshChannel(chanExplicit, chanPskIndex);

    // Messages and neighbours belong to the channel we just left — keeping them
    // would show the rider a conversation they are no longer part of.
    take();
    msgCount = msgHead = unread = 0;
    nodeUsed = 0;
    outboxCount = 0;
    memset(seen, 0, sizeof(seen));
    changed = true;
    give();

    diag::log("mesh: channel '%s'%s key %u -> %.4f MHz hash 0x%02x",
              effectiveChan(), channelFollowsPreset() ? " (follows modem)" : "",
              chanPskIndex, channelFreq(), chanHash);
    if (meshEnabled) {
        radioUp = startRadio();          // the name moves the frequency slot
        nextNodeInfoMs = millis() + 5000;
    }
}

uint32_t queueText(uint32_t dest, const char* text, uint8_t channel) {
    if (!radioUp || !meshEnabled || !text || !text[0]) return 0;
    if (channel >= mesh::MAX_CHANNELS) return 0;
    size_t n = strnlen(text, mesh::MAX_TEXT_LEN + 1);
    if (n > mesh::MAX_TEXT_LEN) n = mesh::MAX_TEXT_LEN;

    take();
    // A direct message asks for an acknowledgement; a broadcast cannot have one.
    const bool wantAck = (dest != BROADCAST_ADDR);
    const uint32_t id = enqueue(dest, mesh::PORT_TEXT, (const uint8_t*)text, n,
                                wantAck, 0, channel);
    if (id) {
        Message* m = pushMessage();
        m->id = id;
        m->from = myNodeNum;
        m->to = dest;
        m->utc = nowUtc();
        m->ms = millis();
        m->outgoing = true;
        m->channel = channel;
        m->status = TX_PENDING;
        memcpy(m->text, text, n);
        m->text[n] = 0;
    }
    give();
    if (id && wake) xSemaphoreGive(wake);    // send it now, not at the next tick
    if (!id) diag::log("mesh: outbox full — message not queued");
    return id;
}

int messageCount() {
    take();
    const int n = msgCount;
    give();
    return n;
}

bool messageAt(int index, Message& out) {
    if (!msgs || index < 0) return false;
    take();
    bool ok = false;
    if (index < msgCount) {
        // Index 0 is the oldest still held. Once the ring has wrapped, that is
        // msgHead — the slot the next message will overwrite.
        const int first = (msgHead - msgCount + MESH_MSG_HISTORY) % MESH_MSG_HISTORY;
        out = msgs[(first + index) % MESH_MSG_HISTORY];
        ok = true;
    }
    give();
    return ok;
}

int nodeCount() {
    take();
    const int n = nodeUsed;
    give();
    return n;
}

bool nodeAt(int index, Node& out) {
    if (index < 0) return false;
    take();
    bool ok = false;
    if (index < nodeUsed) {
        out = nodes[index];
        ok = true;
    }
    give();
    return ok;
}

bool nodeName(uint32_t num, char* out, size_t cap) {
    if (!cap) return false;
    out[0] = 0;
    take();
    bool found = false;
    for (int i = 0; i < nodeUsed; ++i) {
        if (nodes[i].num == num && nodes[i].longName[0]) {
            snprintf(out, cap, "%s", nodes[i].longName);
            found = true;
            break;
        }
    }
    give();
    return found;
}

int unreadCount() { return unread; }
void markAllRead() { unread = 0; }

bool takeChanged() {
    take();
    const bool c = changed;
    changed = false;
    give();
    return c;
}

Stats stats() { return st; }

}  // namespace mesh_service
