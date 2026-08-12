#pragma once

// Meshtastic node: owns the SX1262, speaks the mesh, and keeps the messages and
// neighbour names the phone reads over BLE.
//
// Scope is text chat. The device joins a channel, sends and receives
// TEXT_MESSAGE_APP packets, acknowledges direct messages, and learns names from
// NODEINFO_APP so a sender shows up as "Alex" rather than "!a4c1380c". It does
// NOT rebroadcast other people's traffic (so it is a leaf on the mesh, not a
// router), does not send position, and does not do PKI-encrypted direct
// messages — see docs/meshtastic.md for what that means in practice.
//
// One task owns the radio. Everything a second task may touch (the message ring,
// the node table, the outbox) is behind a mutex and copied out, never handed out
// by pointer, so the BLE server can read state mid-transfer without racing.

#include <cstddef>
#include <cstdint>

#include "config.h"
#include "mesh_proto.h"

namespace mesh_service {

// Delivery state of something we sent. A broadcast never gets past SENT — there
// is nobody in particular to acknowledge it.
enum TxStatus : uint8_t {
    TX_PENDING = 0,   // queued, waiting for a clear channel
    TX_SENT    = 1,   // on the air
    TX_ACKED   = 2,   // the addressee acknowledged it
    TX_FAILED  = 3,
};

struct Message {
    uint32_t id = 0;
    uint32_t from = 0;
    uint32_t to = 0;
    uint32_t utc = 0;        // 0 when the device has no wall clock yet
    uint32_t ms = 0;         // millis() at the time, so age is always knowable
    int8_t   rssi = 0;
    int8_t   snr = 0;
    bool     outgoing = false;
    uint8_t  status = TX_PENDING;
    uint8_t  hops = 0;       // hops taken to reach us (0 = heard directly)
    char     text[mesh::MAX_TEXT_LEN + 1] = {};
};

struct Node {
    uint32_t num = 0;
    char     longName[40] = {};
    char     shortName[8] = {};
    uint32_t lastHeardMs = 0;
    uint32_t lastHeardUtc = 0;
    int8_t   snr = 0;
    int8_t   rssi = 0;
    uint8_t  hops = 0;
};

struct Stats {
    uint32_t rx = 0;              // frames that decoded on our channel
    uint32_t rxDropped = 0;       // CRC failures and undecodable frames
    uint32_t rxOtherChannel = 0;  // heard, but not our channel
    uint32_t rxDuplicate = 0;     // already seen this (sender, id)
    uint32_t tx = 0;
    uint32_t txFailed = 0;
    uint32_t acksRx = 0;
};

// Sets up the node identity and message storage, and brings the radio up if mesh
// messaging is enabled. Returns false only if the service cannot work at all (no
// PSRAM for the message ring) — a radio that did not answer, or one deliberately
// switched off, still returns true, because the phone can enable it later and the
// task has to be there to service it. Ask radioOk() for the radio itself.
bool begin();

// FreeRTOS task: services the radio and the outbox.
void task(void* arg);

bool radioOk();
bool enabled();
void setEnabled(bool on);

uint32_t nodeNum();
float frequencyMHz();
const char* channelName();
const char* longName();
const char* shortName();

// Renames this node and re-announces it to the mesh. Persisted.
void setNames(const char* longName, const char* shortName);

// Moves to another channel. The name feeds both the channel hash AND the
// frequency slot, so this retunes the radio; pskIndex picks one of Meshtastic's
// ten well-known keys (1 = the default channel's).
void setChannel(const char* name, uint8_t pskIndex);
uint8_t channelPskIndex();

// Queues a text message and returns its packet id (0 if the outbox is full, the
// text is empty, or the radio is down). Safe to call from another task.
uint32_t queueText(uint32_t dest, const char* text);

// Copy out state one entry at a time, index 0 = oldest. Deliberately not a
// bulk snapshot: the whole message ring is ~11 KB, far more than the BLE server
// task's stack, and it is only ever read to be streamed out one notification per
// message anyway.
//
// A message arriving mid-stream rotates the ring, so a reader walking the indices
// can see an entry twice or miss one. Consumers key on Message::id for that
// reason; nothing here is a stable cursor.
int messageCount();
bool messageAt(int index, Message& out);
int nodeCount();
bool nodeAt(int index, Node& out);

// Name for a node number, "" if we have never heard its NodeInfo.
bool nodeName(uint32_t num, char* out, size_t cap);

int unreadCount();
void markAllRead();

// True once (and cleared) when a message has arrived or a send changed state, so
// the BLE server can push it to the phone without polling the whole list.
bool takeChanged();

Stats stats();

}  // namespace mesh_service
