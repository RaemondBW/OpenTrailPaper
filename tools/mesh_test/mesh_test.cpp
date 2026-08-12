// Host tests for the Meshtastic wire format in src/mesh_proto.cpp.
//
// These are interop tests, not unit tests. Every number checked here is a value
// the reference implementation produces, so a failure means other nodes cannot
// hear us (or we cannot hear them) — a class of bug that is invisible on the
// bench without a second radio, because a wrongly-keyed or wrongly-tuned device
// simply sits there receiving nothing.

#include <cstdio>
#include <cstring>
#include <cmath>

#include "mesh_proto.h"

static int failures = 0;

static void check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static void checkHex(const uint8_t* got, const uint8_t* want, size_t n,
                     const char* what) {
    const bool ok = memcmp(got, want, n) == 0;
    printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        failures++;
        printf("      got ");
        for (size_t i = 0; i < n; ++i) printf("%02x", got[i]);
        printf("\n      want ");
        for (size_t i = 0; i < n; ++i) printf("%02x", want[i]);
        printf("\n");
    }
}

// The block cipher, against an independent implementation.
//
// ctrCrypt XORs its input with AES(counter), so encrypting an all-zero block
// hands back the raw keystream — one block encryption, exposed. The counter is
// all zero when packetId, fromNode and extraNonce are, which makes the expected
// value simply AES-ECB(key, 0…0). Both vectors below came out of OpenSSL:
//
//   openssl enc -aes-128-ecb -K 000102030405060708090a0b0c0d0e0f -nopad
//   openssl enc -aes-256-ecb -K 000102…1f -nopad
//
// over 16 zero bytes. A wrong key schedule — the easiest thing to get wrong in
// AES-256, which mixes in an extra SubWord — fails here rather than on the air.
static void testAesKnownAnswer() {
    const uint8_t key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                             0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    const uint8_t want[16] = {0xc6, 0xa1, 0x3b, 0x37, 0x87, 0x8f, 0x5b, 0x82,
                              0x6f, 0x4f, 0x81, 0x62, 0xa1, 0xc8, 0xd8, 0x79};
    uint8_t block[16] = {};
    mesh::ctrCrypt(key, 16, 0, 0, 0, block, sizeof(block));
    checkHex(block, want, 16, "AES-128 keystream matches OpenSSL");

    const uint8_t key32[32] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
        0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
    const uint8_t want32[16] = {0xf2, 0x90, 0x00, 0xb6, 0x2a, 0x49, 0x9f, 0xd0,
                                0xa9, 0xf3, 0x9a, 0x6a, 0xdd, 0x2e, 0x77, 0x80};
    uint8_t block32[16] = {};
    mesh::ctrCrypt(key32, 32, 0, 0, 0, block32, sizeof(block32));
    checkHex(block32, want32, 16, "AES-256 keystream matches OpenSSL");
}

static void testCtrRoundTrip() {
    uint8_t psk[16];
    mesh::defaultPsk(1, psk);
    const char* msg = "the quick brown fox jumps over a lazy dog, and then some";
    uint8_t buf[128];
    const size_t n = strlen(msg);
    memcpy(buf, msg, n);
    mesh::ctrCrypt(psk, 16, 0xdeadbeef, 0x12345678, 0, buf, n);
    check(memcmp(buf, msg, n) != 0, "CTR actually changed the bytes");
    // Spanning several blocks is the case where a wrong counter increment shows
    // up: the first 16 bytes would still round-trip.
    check(n > 32, "CTR test payload spans multiple blocks");
    mesh::ctrCrypt(psk, 16, 0xdeadbeef, 0x12345678, 0, buf, n);
    check(memcmp(buf, msg, n) == 0, "CTR decrypts what it encrypted");

    // The nonce binds a payload to its sender and packet id. Decrypting with
    // either one wrong must produce garbage, or replayed packets would decode.
    memcpy(buf, msg, n);
    mesh::ctrCrypt(psk, 16, 0xdeadbeef, 0x12345678, 0, buf, n);
    mesh::ctrCrypt(psk, 16, 0xdeadbeef, 0x12345679, 0, buf, n);
    check(memcmp(buf, msg, n) != 0, "wrong packet id does not decrypt");
}

static void testDefaultPsk() {
    const uint8_t want1[16] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
                               0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};
    uint8_t psk[16];
    mesh::defaultPsk(1, psk);
    checkHex(psk, want1, 16, "default channel PSK (\"AQ==\")");
    // Byte N selects the same key with the last byte advanced by N-1.
    mesh::defaultPsk(2, psk);
    check(psk[15] == 0x02 && psk[0] == 0xd4, "PSK index 2 advances the last byte");
}

// The two hashes that decide where a default node listens. Both are checked
// against the published behaviour of a stock US node on the primary channel:
// LongFast lands on slot 19 of 104, which is 906.875 MHz.
static void testChannelPlacement() {
    const uint32_t n = mesh::channelCount(250.0f, 902.0f, 928.0f, 0.0f);
    check(n == 104, "US 915 / 250 kHz gives 104 channel slots");

    const uint32_t slot = mesh::nameHash("LongFast") % n;
    printf("      LongFast djb2=%u slot=%u\n", mesh::nameHash("LongFast"), slot);
    check(slot == 19, "LongFast hashes to slot 19");

    const float f = mesh::channelFrequencyMHz("LongFast", 250.0f, 902.0f, 928.0f,
                                              0.0f);
    printf("      frequency = %.4f MHz\n", f);
    check(std::fabs(f - 906.875f) < 0.0005f, "LongFast centre is 906.875 MHz");

    uint8_t psk[16];
    mesh::defaultPsk(1, psk);
    const uint8_t h = mesh::channelHash("LongFast", psk, 16);
    printf("      channel hash = 0x%02x\n", h);
    // XOR-fold of "LongFast" is 0x0a, of the default PSK 0x02, so the hash a
    // stock node stamps on its primary channel is 0x08.
    check(h == 0x08, "LongFast channel hash is 0x08");
    check(mesh::channelHash("LongFast", psk, 16) !=
              mesh::channelHash("LongSlow", psk, 16),
          "a different channel name gives a different hash");

    // MediumFast, the preset this firmware currently ships (see config.h). Pinned
    // here because switching preset is the one change that silently moves the
    // radio: the preset name IS the default channel name, so it changes the
    // frequency slot and the channel byte as well as the spreading factor.
    const uint32_t mfSlot = mesh::nameHash("MediumFast") % n;
    const float mf = mesh::channelFrequencyMHz("MediumFast", 250.0f, 902.0f,
                                               928.0f, 0.0f);
    printf("      MediumFast slot=%u frequency=%.4f MHz hash=0x%02x\n", mfSlot, mf,
           mesh::channelHash("MediumFast", psk, 16));
    check(mfSlot == 44, "MediumFast hashes to slot 44");
    check(std::fabs(mf - 913.125f) < 0.0005f, "MediumFast centre is 913.125 MHz");
    check(mesh::channelHash("MediumFast", psk, 16) == 0x1f,
          "MediumFast channel hash is 0x1f");
    // The two presets are genuinely different radios, not just different speeds.
    check(std::fabs(mf - f) > 1.0f,
          "changing preset moves the frequency, so presets cannot interoperate");
}

static void testHeader() {
    mesh::Header h;
    h.dest = mesh::BROADCAST_ADDR;
    h.sender = 0x1a2b3c4d;
    h.id = 0x99887766;
    h.hopLimit = 3;
    h.hopStart = 3;
    h.wantAck = true;
    h.channelHash = 0x68;

    uint8_t buf[mesh::HEADER_LEN];
    mesh::encodeHeader(h, buf);

    // Little-endian u32s, then the flag byte. hop_limit 3 | want_ack 0x08 |
    // hop_start 3 << 5 = 0x60  ->  0x6b.
    const uint8_t want[16] = {0xff, 0xff, 0xff, 0xff, 0x4d, 0x3c, 0x2b, 0x1a,
                              0x66, 0x77, 0x88, 0x99, 0x6b, 0x68, 0x00, 0x00};
    checkHex(buf, want, 16, "header bytes match the reference layout");

    mesh::Header back;
    check(mesh::decodeHeader(buf, sizeof(buf), back), "header decodes");
    check(back.dest == h.dest && back.sender == h.sender && back.id == h.id &&
              back.hopLimit == 3 && back.hopStart == 3 && back.wantAck &&
              !back.viaMqtt && back.channelHash == 0x68,
          "header round-trips every field");

    uint8_t zeroSender[16] = {};
    zeroSender[0] = 0xff;
    check(!mesh::decodeHeader(zeroSender, 16, back),
          "a zero sender is rejected");
    check(!mesh::decodeHeader(buf, 15, back), "a short header is rejected");
}

static void testDataProto() {
    mesh::Data d;
    d.portnum = mesh::PORT_TEXT;
    const char* text = "on the ridge, 20 min out";
    d.payloadLen = strlen(text);
    memcpy(d.payload, text, d.payloadLen);

    uint8_t buf[256];
    const size_t n = mesh::encodeData(d, buf, sizeof(buf));
    check(n > 0, "Data encodes");
    // field 1 varint = 0x08 0x01, field 2 bytes = 0x12 <len> <text>
    check(buf[0] == 0x08 && buf[1] == 0x01 && buf[2] == 0x12 &&
              buf[3] == (uint8_t)d.payloadLen,
          "Data field tags are TEXT_MESSAGE_APP + payload");
    check(n == 4 + d.payloadLen, "Data has no unexpected fields on the wire");

    mesh::Data back;
    check(mesh::decodeData(buf, n, back), "Data decodes");
    check(back.portnum == mesh::PORT_TEXT &&
              back.payloadLen == d.payloadLen &&
              memcmp(back.payload, text, d.payloadLen) == 0,
          "Data round-trips the text");

    // An ack: routing payload with the acknowledged id in request_id.
    mesh::Data ack;
    ack.portnum = mesh::PORT_ROUTING;
    ack.payloadLen = mesh::encodeRouting(0, ack.payload, sizeof(ack.payload));
    ack.requestId = 0x99887766;
    const size_t an = mesh::encodeData(ack, buf, sizeof(buf));
    check(an > 0, "ack Data encodes");
    mesh::Data ackBack;
    check(mesh::decodeData(buf, an, ackBack), "ack Data decodes");
    uint32_t err = 0xffff;
    check(mesh::decodeRoutingError(ackBack.payload, ackBack.payloadLen, err) &&
              err == 0,
          "ack carries error_reason NONE");
    check(ackBack.requestId == 0x99887766, "ack carries the request id");

    // Unknown fields must be skipped by wire type, not guessed at. Field 9
    // (bitfield, a varint newer firmware sets) followed by the portnum.
    const uint8_t withUnknown[] = {0x48, 0x01, 0x08, 0x01};
    mesh::Data fwd;
    check(mesh::decodeData(withUnknown, sizeof(withUnknown), fwd) &&
              fwd.portnum == mesh::PORT_TEXT,
          "an unknown field is skipped, not misread");

    // Truncated input must be refused rather than half-decoded.
    const uint8_t truncated[] = {0x12, 0x40, 0x41};   // says 64 bytes, has 1
    mesh::Data bad;
    check(!mesh::decodeData(truncated, sizeof(truncated), bad),
          "a truncated length-delimited field is rejected");
}

static void testUserProto() {
    mesh::User u;
    mesh::nodeIdString(0x1a2b3c4d, u.id, sizeof(u.id));
    check(strcmp(u.id, "!1a2b3c4d") == 0, "node id string format");
    snprintf(u.longName, sizeof(u.longName), "OpenTrailPaper");
    snprintf(u.shortName, sizeof(u.shortName), "OTP");

    uint8_t buf[128];
    const size_t n = mesh::encodeUser(u, buf, sizeof(buf));
    check(n > 0, "User encodes");
    mesh::User back;
    check(mesh::decodeUser(buf, n, back), "User decodes");
    check(strcmp(back.id, u.id) == 0 &&
              strcmp(back.longName, "OpenTrailPaper") == 0 &&
              strcmp(back.shortName, "OTP") == 0,
          "User round-trips names");

    // A long_name longer than our field must truncate, not overflow.
    uint8_t big[200];
    size_t p = 0;
    big[p++] = 0x12;          // field 2, length-delimited
    big[p++] = 100;
    for (int i = 0; i < 100; ++i) big[p++] = 'x';
    mesh::User longName;
    check(mesh::decodeUser(big, p, longName), "over-long name decodes");
    check(strlen(longName.longName) == sizeof(longName.longName) - 1,
          "over-long name is truncated to the field size");
}

// The full send path, decoded back the way a peer would: header in the clear,
// payload decrypted with the channel key, Data parsed out of it.
static void testEndToEnd() {
    uint8_t psk[16];
    mesh::defaultPsk(1, psk);

    mesh::Data d;
    d.portnum = mesh::PORT_TEXT;
    const char* text = "flat tire at the top of the fire road";
    d.payloadLen = strlen(text);
    memcpy(d.payload, text, d.payloadLen);

    uint8_t frame[mesh::MAX_RADIO_LEN];
    mesh::Header h;
    h.dest = mesh::BROADCAST_ADDR;
    h.sender = 0x1a2b3c4d;
    h.id = 0x0badf00d;
    h.hopLimit = 3;
    h.hopStart = 3;
    h.channelHash = mesh::channelHash("LongFast", psk, 16);
    mesh::encodeHeader(h, frame);

    const size_t plen = mesh::encodeData(d, frame + mesh::HEADER_LEN,
                                        sizeof(frame) - mesh::HEADER_LEN);
    check(plen > 0, "end-to-end: payload encoded");
    mesh::ctrCrypt(psk, 16, h.sender, h.id, 0, frame + mesh::HEADER_LEN, plen);
    const size_t frameLen = mesh::HEADER_LEN + plen;

    // Receive side.
    mesh::Header rh;
    check(mesh::decodeHeader(frame, frameLen, rh), "end-to-end: header decoded");
    check(rh.channelHash == mesh::channelHash("LongFast", psk, 16),
          "end-to-end: channel hash identifies the channel");
    uint8_t payload[mesh::MAX_PAYLOAD_LEN];
    const size_t rlen = frameLen - mesh::HEADER_LEN;
    memcpy(payload, frame + mesh::HEADER_LEN, rlen);
    mesh::ctrCrypt(psk, 16, rh.sender, rh.id, 0, payload, rlen);
    mesh::Data rd;
    check(mesh::decodeData(payload, rlen, rd), "end-to-end: Data decoded");
    check(rd.portnum == mesh::PORT_TEXT, "end-to-end: port is TEXT_MESSAGE_APP");
    check(rd.payloadLen == strlen(text) &&
              memcmp(rd.payload, text, rd.payloadLen) == 0,
          "end-to-end: text survived the round trip");

    // A longest-allowed message must still fit inside one radio frame.
    mesh::Data big;
    big.portnum = mesh::PORT_TEXT;
    big.payloadLen = mesh::MAX_TEXT_LEN;
    memset(big.payload, 'A', big.payloadLen);
    uint8_t bigOut[mesh::MAX_PAYLOAD_LEN];
    const size_t bn = mesh::encodeData(big, bigOut, sizeof(bigOut));
    check(bn > 0 && mesh::HEADER_LEN + bn <= mesh::MAX_RADIO_LEN,
          "a max-length text fits one radio frame");

    // And a payload that cannot fit must fail loudly rather than truncate.
    uint8_t tiny[8];
    check(mesh::encodeData(big, tiny, sizeof(tiny)) == 0,
          "encodeData refuses a buffer it would overrun");
}

int main() {
    printf("== Meshtastic wire format ==\n");
    testAesKnownAnswer();
    testCtrRoundTrip();
    testDefaultPsk();
    testChannelPlacement();
    testHeader();
    testDataProto();
    testUserProto();
    testEndToEnd();
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all passed",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
