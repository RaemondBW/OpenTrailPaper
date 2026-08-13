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

#include "config.h"
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

    // A second named channel, pinned because "MediumFast" is a name people
    // actually use — Meshtastic offers a preset by that name, and a channel named
    // after it is a different frequency from the default, not a faster version of
    // it. (The MODEM of the same name is a separate setting; see testPresets.)
    const uint32_t mfSlot = mesh::nameHash("MediumFast") % n;
    const float mf = mesh::channelFrequencyMHz("MediumFast", 250.0f, 902.0f,
                                               928.0f, 0.0f);
    printf("      MediumFast slot=%u frequency=%.4f MHz hash=0x%02x\n", mfSlot, mf,
           mesh::channelHash("MediumFast", psk, 16));
    check(mfSlot == 44, "MediumFast hashes to slot 44");
    check(std::fabs(mf - 913.125f) < 0.0005f, "MediumFast centre is 913.125 MHz");
    check(mesh::channelHash("MediumFast", psk, 16) == 0x1f,
          "MediumFast channel hash is 0x1f");
    // Two channel names are two frequencies — nodes on them cannot hear each
    // other at all, however well their modems match.
    check(std::fabs(mf - f) > 1.0f,
          "two channel names land on different frequencies");
}

// The modem preset table. Index 0 must stay LongFast (it is the default a device
// with no stored choice comes up on), and the indices are persisted in NVS and
// sent over BLE, so the order is a contract.
static void testPresets() {
    check(mesh::PRESET_COUNT >= 1, "there is at least one preset");
    check(strcmp(mesh::kPresets[0].name, "LongFast") == 0,
          "preset 0 is LongFast (the Meshtastic default)");
    check(mesh::kPresets[0].sf == 11 && mesh::kPresets[0].bwKhz == 250.0f &&
              mesh::kPresets[0].cr == 5,
          "LongFast is SF11 / 250 kHz / CR4:5");
    check(mesh::presetIndexByName("MediumFast") == 2, "MediumFast is index 2");
    check(mesh::presetIndexByName("mediumfast") == 2, "preset lookup ignores case");
    check(mesh::presetIndexByName("Nonsense") < 0, "an unknown preset is rejected");
    check(mesh::presetIndexByName("Medium") < 0,
          "a prefix is not a match (Medium is not MediumFast)");
    // An index from a newer firmware must not drive the radio with garbage.
    check(strcmp(mesh::preset(99).name, "LongFast") == 0,
          "an out-of-range preset falls back to LongFast");
    check(strcmp(mesh::preset(-1).name, "LongFast") == 0,
          "a negative preset falls back to LongFast");

    // The interop rule this table exists to serve: an unnamed primary channel takes
    // the PRESET's name, so a node on MediumFast is on MediumFast's slot. Firmware
    // that pinned the channel to "LongFast" and let the preset move independently
    // would sit on 906.875 while a stock MediumFast node sat on 913.125.
    for (int i = 0; i < mesh::PRESET_COUNT; ++i) {
        const mesh::ModemPreset& p = mesh::kPresets[i];
        const float f = mesh::channelFrequencyMHz(p.name, p.bwKhz, 902.0f, 928.0f,
                                                  0.0f);
        printf("      %-11s -> %.4f MHz\n", p.name, f);
        check(f >= 902.0f && f <= 928.0f,
              "every preset's own name lands inside the band");
    }
    check(std::fabs(mesh::channelFrequencyMHz("MediumFast", 250.0f, 902.0f, 928.0f,
                                              0.0f) - 913.125f) < 0.0005f,
          "an unnamed channel on MediumFast is on 913.125, not LongFast's slot");

    // Bandwidth is part of a preset, and slots are bandwidth-wide — so a preset
    // that changes the bandwidth moves the frequency even on the same channel.
    const float at250 = mesh::channelFrequencyMHz("LongFast", 250.0f, 902.0f,
                                                  928.0f, 0.0f);
    const float at500 = mesh::channelFrequencyMHz("LongFast", 500.0f, 902.0f,
                                                  928.0f, 0.0f);
    printf("      LongFast at 250 kHz = %.4f, at 500 kHz = %.4f MHz\n", at250, at500);
    check(at250 != at500,
          "bandwidth changes the frequency slot, so the preset can retune too");
}

// The Bay Area Mesh's published settings, pinned so a change here cannot quietly
// take the device off a real mesh someone is using.
//   https://bayme.sh/docs/getting-started/recommended-settings/
//
// Their recommendation is: region US, preset "Medium Range Fast", primary channel
// name blank, key AQ==, frequency slot 45. The slot is marked optional there, and
// this is why — a blank channel name takes the preset's name, and "MediumFast"
// hashes to exactly that slot, so the default lands on it without being told.
static void testBayMeshSettings() {
    const int idx = mesh::presetIndexByName("MediumFast");
    check(idx >= 0, "BayMesh: the MediumFast preset exists");
    const mesh::ModemPreset& p = mesh::preset(idx);
    check(p.sf == 9 && p.bwKhz == 250.0f, "BayMesh: MediumFast is SF9 / 250 kHz");

    // Slot numbers in the Meshtastic UI are 1-based; the code uses slot - 1.
    const uint32_t slot = mesh::nameHash(p.name) %
                          mesh::channelCount(p.bwKhz, 902.0f, 928.0f, 0.0f);
    printf("      BayMesh: slot %u (UI slot %u)\n", slot, slot + 1);
    check(slot + 1 == 45, "BayMesh: a blank channel on MediumFast is UI slot 45");

    const float f = mesh::channelFrequencyMHz(p.name, p.bwKhz, 902.0f, 928.0f, 0.0f);
    check(std::fabs(f - 913.125f) < 0.0005f, "BayMesh: slot 45 is 913.125 MHz");

    // Key AQ== is the single-byte default PSK, index 1.
    uint8_t psk[16];
    mesh::defaultPsk(1, psk);
    const uint8_t h = mesh::channelHash(p.name, psk, 16);
    printf("      BayMesh: channel hash 0x%02x\n", h);
    check(h == 0x1f, "BayMesh: channel hash is 0x1f");

    // Their personal/chat recommendation is 6, and the header field is 3 bits.
    check(MESH_HOP_LIMIT <= 7, "BayMesh: hop limit fits the 3-bit header field");
    check(MESH_HOP_LIMIT == 6, "BayMesh: hop limit is their personal-node value");
}

// meshtastic.ChannelSet — the payload of a channel-share QR code.
//
// The bytes below are pinned rather than round-tripped, because round-tripping
// only proves this file agrees with itself. base64url of 0a 03 12 01 01 is
// "CgMSAQE", and https://meshtastic.org/e/#CgMSAQE is Meshtastic's own share URL
// for an unconfigured default channel — so matching these bytes is matching the
// real thing. (Cross-checked with Python's base64.urlsafe_b64encode.)
static void testChannelSet() {
    mesh::ChannelSettings def;
    def.psk[0] = 0x01;          // the single-byte well-known key, "AQ=="
    def.pskLen = 1;

    uint8_t buf[128];
    size_t n = mesh::encodeChannelSet(&def, 1, buf, sizeof(buf));
    const uint8_t want[] = {0x0a, 0x03, 0x12, 0x01, 0x01};
    check(n == sizeof(want), "ChannelSet for the default channel is 5 bytes");
    if (n == sizeof(want)) checkHex(buf, want, n, "ChannelSet matches meshtastic.org/e/#CgMSAQE");

    // A private channel: a name and a real 256-bit key.
    mesh::ChannelSettings priv;
    snprintf(priv.name, sizeof(priv.name), "Trail");
    for (int i = 0; i < 32; ++i) priv.psk[i] = (uint8_t)i;
    priv.pskLen = 32;
    n = mesh::encodeChannelSet(&priv, 1, buf, sizeof(buf));
    check(n == 43, "ChannelSet with a 32-byte key and a 5-char name is 43 bytes");
    check(buf[0] == 0x0a && buf[1] == 41 && buf[2] == 0x12 && buf[3] == 32,
          "private channel encodes psk before name, as Meshtastic does");

    mesh::ChannelSettings back[mesh::MAX_CHANNELS];
    int count = 0;
    check(mesh::decodeChannelSet(buf, n, back, mesh::MAX_CHANNELS, count),
          "ChannelSet decodes");
    check(count == 1, "one channel came back");
    check(strcmp(back[0].name, "Trail") == 0, "channel name round-trips");
    check(back[0].pskLen == 32 && back[0].psk[31] == 31, "32-byte key round-trips");

    // Several channels in one set, which is what sharing a whole config looks like.
    mesh::ChannelSettings many[3];
    many[0] = def;
    many[1] = priv;
    snprintf(many[2].name, sizeof(many[2].name), "Second");
    many[2].psk[0] = 0x02;
    many[2].pskLen = 1;
    n = mesh::encodeChannelSet(many, 3, buf, sizeof(buf));
    check(n > 0, "a three-channel set encodes");
    count = 0;
    check(mesh::decodeChannelSet(buf, n, back, mesh::MAX_CHANNELS, count) &&
              count == 3 && strcmp(back[2].name, "Second") == 0,
          "a three-channel set round-trips in order");

    // More channels than we hold must drop the extras rather than fail: a QR from
    // someone with a fuller config should still give us the ones we can keep.
    count = 0;
    check(mesh::decodeChannelSet(buf, n, back, 2, count) && count == 2,
          "a set larger than our table keeps what fits");

    // A key length we cannot use must be refused outright, not silently truncated
    // into a different key.
    uint8_t bad[64];
    size_t bn = 0;
    bad[bn++] = 0x0a; bad[bn++] = 12;
    bad[bn++] = 0x12; bad[bn++] = 10;
    for (int i = 0; i < 10; ++i) bad[bn++] = 0xAA;
    mesh::ChannelSettings one;
    check(mesh::decodeChannelSettings(bad + 2, 12, one) && one.pskLen == 10,
          "a 10-byte psk decodes (length is checked when it is expanded)");
    uint8_t key[mesh::MAX_PSK_LEN];
    check(mesh::expandPsk(one.psk, one.pskLen, key) == 0,
          "a 10-byte psk expands to nothing usable");

    // The three lengths that are usable.
    check(mesh::expandPsk(def.psk, 1, key) == 16, "a 1-byte psk expands to 16");
    check(mesh::expandPsk(priv.psk, 32, key) == 32, "a 32-byte psk passes through");
    check(mesh::expandPsk(priv.psk, 0, key) == 0, "an empty psk is unencrypted");

    // The channel hash is computed over the STORED psk, not the expansion —
    // getting that wrong would put a default channel on the wrong hash.
    uint8_t expanded[16];
    mesh::defaultPsk(1, expanded);
    check(mesh::channelHash("LongFast", def.psk, 1) !=
              mesh::channelHash("LongFast", expanded, 16),
          "stored and expanded keys hash differently, so the choice matters");
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

// meshtastic.Position. The coordinates are sfixed32 — wire type 5, little-endian
// two's complement — which is the one place this codec reads a fixed-width field
// rather than a varint, and getting it wrong turns a west longitude into four
// billion rather than failing visibly.
//
// Note putTag(): field numbers above 15 need a multi-byte varint tag (sats_in_view
// is field 19, so 19<<3 = 152 does NOT fit in one byte). Writing those by hand is
// how the first version of this test managed to fail against a correct decoder.
static void putTag(uint8_t* buf, size_t& n, uint32_t field, uint8_t wire) {
    uint64_t v = ((uint64_t)field << 3) | wire;
    while (v >= 0x80) { buf[n++] = (uint8_t)(v | 0x80); v >>= 7; }
    buf[n++] = (uint8_t)v;
}

static void testPosition() {
    // San Francisco: 37.7764, -122.4346. The negative longitude is the point.
    const int32_t latE7 = 377764000;
    const int32_t lonE7 = -1224346000;
    uint8_t buf[64];
    size_t n = 0;
    putTag(buf, n, 1, 5);               // latitude_i, sfixed32
    memcpy(buf + n, &latE7, 4); n += 4;
    putTag(buf, n, 2, 5);               // longitude_i, sfixed32
    memcpy(buf + n, &lonE7, 4); n += 4;
    putTag(buf, n, 3, 0);               // altitude, varint
    buf[n++] = 52;
    putTag(buf, n, 19, 0);              // sats_in_view
    buf[n++] = 9;

    mesh::Position pos;
    check(mesh::decodePosition(buf, n, pos), "Position decodes");
    check(pos.valid, "Position is valid");
    printf("      %.5f, %.5f  %dm  %u sats  %u bits\n", pos.latitude,
           pos.longitude, (int)pos.altitudeM, pos.satsInView, pos.precisionBits);
    check(std::fabs(pos.latitude - 37.7764) < 1e-6, "latitude decodes");
    check(std::fabs(pos.longitude - (-122.4346)) < 1e-6,
          "a negative longitude decodes as negative");
    check(pos.altitudeM == 52, "altitude decodes");
    check(pos.satsInView == 9, "satellite count decodes");
    check(pos.precisionBits == 32, "precision defaults to full when absent");

    // A node with no fix broadcasts zeroes, and "0,0" must not read as a place.
    uint8_t zero[16];
    size_t zn = 0;
    const int32_t z = 0;
    putTag(zero, zn, 1, 5);
    memcpy(zero + zn, &z, 4); zn += 4;
    putTag(zero, zn, 2, 5);
    memcpy(zero + zn, &z, 4); zn += 4;
    mesh::Position nofix;
    check(mesh::decodePosition(zero, zn, nofix) && !nofix.valid,
          "0,0 is treated as no fix rather than the null island");

    // A Position carrying only a satellite count is not a location.
    uint8_t noCoords[8];
    size_t ncn = 0;
    putTag(noCoords, ncn, 19, 0);
    noCoords[ncn++] = 7;
    mesh::Position bare;
    check(mesh::decodePosition(noCoords, ncn, bare) && !bare.valid,
          "a Position with no coordinates is not valid");

    // A deliberately blurred position still decodes, and says so.
    uint8_t blurred[32];
    size_t bn = 0;
    putTag(blurred, bn, 1, 5);
    memcpy(blurred + bn, &latE7, 4); bn += 4;
    putTag(blurred, bn, 2, 5);
    memcpy(blurred + bn, &lonE7, 4); bn += 4;
    putTag(blurred, bn, 23, 0);         // precision_bits
    blurred[bn++] = 13;
    mesh::Position coarse;
    check(mesh::decodePosition(blurred, bn, coarse) && coarse.valid &&
              coarse.precisionBits == 13,
          "a reduced-precision position decodes and reports its precision");

    // Truncated fixed32 must be refused, not read past the end.
    const uint8_t truncated[] = {(1 << 3) | 5, 0x01, 0x02};
    mesh::Position cut;
    check(!mesh::decodePosition(truncated, sizeof(truncated), cut),
          "a truncated coordinate is rejected");

    // An out-of-range coordinate that still passed CRC must not plot.
    uint8_t insane[16];
    size_t inN = 0;
    const int32_t hugeLat = 2000000000;   // 200 degrees
    putTag(insane, inN, 1, 5);
    memcpy(insane + inN, &hugeLat, 4); inN += 4;
    putTag(insane, inN, 2, 5);
    memcpy(insane + inN, &lonE7, 4); inN += 4;
    mesh::Position bogus;
    check(mesh::decodePosition(insane, inN, bogus) && !bogus.valid,
          "an out-of-range latitude is rejected");

    // A real node sends fields we do not read, including some above field 15 with
    // two-byte tags. Those must be skipped without disturbing the coordinates.
    uint8_t withExtras[64];
    size_t en = 0;
    putTag(withExtras, en, 12, 0);      // HDOP
    withExtras[en++] = 120;
    putTag(withExtras, en, 1, 5);
    memcpy(withExtras + en, &latE7, 4); en += 4;
    putTag(withExtras, en, 16, 0);      // ground_track
    withExtras[en++] = 90;
    putTag(withExtras, en, 2, 5);
    memcpy(withExtras + en, &lonE7, 4); en += 4;
    mesh::Position mixed;
    check(mesh::decodePosition(withExtras, en, mixed) && mixed.valid &&
              std::fabs(mixed.latitude - 37.7764) < 1e-6,
          "unread fields with multi-byte tags are skipped correctly");
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
    testPresets();
    testBayMeshSettings();
    testChannelSet();
    testHeader();
    testDataProto();
    testUserProto();
    testPosition();
    testEndToEnd();
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all passed",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
