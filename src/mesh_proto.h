#pragma once

// Meshtastic wire format: the 16-byte radio header, the protobuf messages we
// need for text chat, and the two hashes that decide which frequency slot and
// which channel a packet belongs to.
//
// This is a hand-written subset, not generated code. Meshtastic's own .proto set
// is large and pulls in nanopb; text messaging needs four messages (Data, User,
// Routing, and the header itself), all of which are a handful of scalar fields.
// The whole file is host-compilable with no Arduino or ESP-IDF dependency so
// tools/mesh_test can check it against known-good values (see run_mesh_test.sh);
// the numbers below are only right if they agree with the reference
// implementation to the byte, and a host test is the only way to keep them
// honest without a second radio on the bench.
//
// Reference: meshtastic/firmware src/mesh/RadioInterface.{h,cpp},
// CryptoEngine.cpp, and protobufs/meshtastic/{mesh,portnums}.proto.

#include <cstddef>
#include <cstdint>

namespace mesh {

// Anything addressed here is for every node on the channel.
constexpr uint32_t BROADCAST_ADDR = 0xFFFFFFFFu;

constexpr size_t HEADER_LEN = 16;
// The SX1262 FIFO caps a LoRa packet at 255 bytes; Meshtastic sizes its buffers
// to 256 and never fills the last byte.
constexpr size_t MAX_RADIO_LEN = 255;
constexpr size_t MAX_PAYLOAD_LEN = MAX_RADIO_LEN - HEADER_LEN;
// Longest text we will send. The protobuf wrapper costs a few bytes on top and
// the rest of the budget is left as headroom, which also keeps a whole message
// inside one 247-byte BLE notification to the phone.
constexpr size_t MAX_TEXT_LEN = 200;

// Only the port numbers this firmware speaks. The full enum is much longer.
enum PortNum : uint8_t {
    PORT_UNKNOWN  = 0,
    PORT_TEXT     = 1,   // TEXT_MESSAGE_APP  — payload is raw UTF-8
    PORT_POSITION = 3,   // POSITION_APP      — decoded only, never sent
    PORT_NODEINFO = 4,   // NODEINFO_APP      — payload is a User
    PORT_ROUTING  = 5,   // ROUTING_APP       — payload is a Routing (our acks)
};

// The unencrypted part of every packet, in the order it goes on air.
struct Header {
    uint32_t dest = BROADCAST_ADDR;
    uint32_t sender = 0;
    uint32_t id = 0;
    uint8_t  hopLimit = 0;     // hops REMAINING
    uint8_t  hopStart = 0;     // hops the sender started with, for distance
    bool     wantAck = false;
    bool     viaMqtt = false;
    uint8_t  channelHash = 0;
    uint8_t  nextHop = 0;      // 0 = no next-hop hint (we never set one)
    uint8_t  relayNode = 0;    // 0 = unknown relay
};

// Writes exactly HEADER_LEN bytes.
void encodeHeader(const Header& h, uint8_t* out);
bool decodeHeader(const uint8_t* in, size_t len, Header& h);

// meshtastic.Data — the payload every port number is wrapped in.
struct Data {
    uint8_t  portnum = PORT_UNKNOWN;
    uint8_t  payload[MAX_PAYLOAD_LEN] = {};
    size_t   payloadLen = 0;
    bool     wantResponse = false;
    uint32_t dest = 0;
    uint32_t source = 0;
    uint32_t requestId = 0;    // set on an ack: the id being acknowledged
    uint32_t replyId = 0;
};

// Returns the encoded length, or 0 if it would not fit in `cap`.
size_t encodeData(const Data& d, uint8_t* out, size_t cap);
bool decodeData(const uint8_t* in, size_t len, Data& d);

// meshtastic.User — how a node tells the mesh its name.
struct User {
    char    id[16] = {};        // "!aabbccdd", derived from the node number
    char    longName[40] = {};
    char    shortName[8] = {};  // up to 4 glyphs in practice
    uint8_t hwModel = 0;        // HardwareModel; 0 = UNSET
};

size_t encodeUser(const User& u, uint8_t* out, size_t cap);
bool decodeUser(const uint8_t* in, size_t len, User& u);

// meshtastic.Routing, error_reason field only — which is all an ack is.
// NONE (0) is an ACK; anything else is a NAK carrying the reason.
size_t encodeRouting(uint32_t errorReason, uint8_t* out, size_t cap);
bool decodeRoutingError(const uint8_t* in, size_t len, uint32_t& err);

// ---------------------------------------------------------------------------
// Channel identity
// ---------------------------------------------------------------------------

// The byte in the header that says "this packet is on that channel". Receivers
// use it to pick a decryption key without trying them all. Note that it mixes
// the key in, so two channels sharing a name but not a key do not collide.
uint8_t channelHash(const char* name, const uint8_t* psk, size_t pskLen);

// djb2 over the channel name — Meshtastic uses this one (NOT channelHash) to
// pick the frequency slot, so a channel name change moves the radio.
uint32_t nameHash(const char* s);

// Which slot the named channel lands on, and its centre frequency. Slots are
// bandwidth-wide and packed from freqStart; the +bw/2 centres the first one.
uint32_t channelCount(float bwKhz, float freqStartMHz, float freqEndMHz,
                      float spacingMHz);
float channelFrequencyMHz(const char* channelName, float bwKhz,
                          float freqStartMHz, float freqEndMHz,
                          float spacingMHz);

// Meshtastic encodes the well-known channel keys as a single byte 1..10 (the
// default channel's "AQ==" is byte 1). Byte N means the 16-byte default key
// with its last byte advanced by N-1. Writes 16 bytes.
void defaultPsk(uint8_t index, uint8_t out[16]);

// AES-CTR over `data` in place, with Meshtastic's nonce construction:
// [packetId as u64 LE][fromNode as u32 LE][extraNonce as u32 LE]. Encryption and
// decryption are the same call. keyLen must be 16 or 32.
void ctrCrypt(const uint8_t* key, size_t keyLen, uint32_t fromNode,
              uint32_t packetId, uint32_t extraNonce, uint8_t* data,
              size_t len);

// "!aabbccdd" — how a node number is written everywhere in Meshtastic.
void nodeIdString(uint32_t nodeNum, char* out, size_t cap);

}  // namespace mesh
