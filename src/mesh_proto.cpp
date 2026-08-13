#include "mesh_proto.h"

#include <cmath>
#include <cstring>

namespace {

// ---------------------------------------------------------------------------
// Protobuf wire primitives
// ---------------------------------------------------------------------------
//
// Only the wire types the messages below use: varint (0), length-delimited (2)
// and — for Position's coordinates — fixed 32-bit (5). Anything else is skipped by
// its wire type. A writer that runs out of room sets `overflow` and stops
// writing rather than truncating a field in half, so a caller that ignores the
// return value cannot emit a packet that decodes as something else.

struct Writer {
    uint8_t* buf;
    size_t cap;
    size_t len = 0;
    bool overflow = false;

    void raw(uint8_t b) {
        if (len >= cap) { overflow = true; return; }
        buf[len++] = b;
    }
    void varint(uint64_t v) {
        while (v >= 0x80) { raw((uint8_t)(v | 0x80)); v >>= 7; }
        raw((uint8_t)v);
    }
    void tag(uint32_t field, uint8_t wire) { varint(((uint64_t)field << 3) | wire); }

    // Proto3 does not put a default-valued scalar on the wire at all, and
    // Meshtastic relies on that: a Data with want_response=false must be byte
    // identical to one where the field was never set, or the two hash and
    // encrypt differently for no reason.
    void u32(uint32_t field, uint32_t v) {
        if (!v) return;
        tag(field, 0);
        varint(v);
    }
    void boolean(uint32_t field, bool v) {
        if (!v) return;
        tag(field, 0);
        varint(1);
    }
    void bytes(uint32_t field, const uint8_t* p, size_t n) {
        if (!n) return;
        tag(field, 2);
        varint(n);
        if (len + n > cap) { overflow = true; return; }
        memcpy(buf + len, p, n);
        len += n;
    }
    void str(uint32_t field, const char* s) {
        if (!s || !*s) return;
        bytes(field, (const uint8_t*)s, strlen(s));
    }
};

struct Reader {
    const uint8_t* buf;
    size_t len;
    size_t pos = 0;
    bool bad = false;

    bool done() const { return bad || pos >= len; }
    uint64_t varint() {
        uint64_t v = 0;
        int shift = 0;
        while (pos < len) {
            uint8_t b = buf[pos++];
            v |= (uint64_t)(b & 0x7F) << shift;
            if (!(b & 0x80)) return v;
            shift += 7;
            if (shift > 63) break;
        }
        bad = true;
        return 0;
    }
    // Advances past a field whose number we do not handle. Required: an unknown
    // field must be skipped by its wire type, not guessed at, or every field
    // after it is misread.
    void skip(uint8_t wire) {
        switch (wire) {
        case 0: varint(); break;
        case 1: pos += 8; break;
        case 2: { uint64_t n = varint(); pos += (size_t)n; break; }
        case 5: pos += 4; break;
        default: bad = true; break;
        }
        if (pos > len) bad = true;
    }
    // Bounds-checked view of a length-delimited field.
    bool slice(const uint8_t*& p, size_t& n) {
        uint64_t want = varint();
        if (bad || want > len - pos) { bad = true; return false; }
        p = buf + pos;
        n = (size_t)want;
        pos += n;
        return true;
    }
    // Wire type 5. Signed because that is how Meshtastic stores coordinates
    // (sfixed32), and a two's-complement read is what makes a west longitude
    // come back negative rather than as four billion.
    int32_t fixed32() {
        if (pos + 4 > len) { bad = true; return 0; }
        const uint32_t v = (uint32_t)buf[pos] | ((uint32_t)buf[pos + 1] << 8) |
                           ((uint32_t)buf[pos + 2] << 16) |
                           ((uint32_t)buf[pos + 3] << 24);
        pos += 4;
        return (int32_t)v;
    }
    void copyStr(char* out, size_t cap) {
        const uint8_t* p;
        size_t n;
        if (!slice(p, n)) return;
        if (n > cap - 1) n = cap - 1;
        memcpy(out, p, n);
        out[n] = 0;
    }
};

inline void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
inline uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// Header flag bits (RadioInterface.h).
constexpr uint8_t FLAG_HOP_LIMIT = 0x07;
constexpr uint8_t FLAG_WANT_ACK  = 0x08;
constexpr uint8_t FLAG_VIA_MQTT  = 0x10;
constexpr uint8_t FLAG_HOP_START = 0xE0;
constexpr int FLAG_HOP_START_SHIFT = 5;

// ---------------------------------------------------------------------------
// AES-128/256, encryption direction only
// ---------------------------------------------------------------------------
//
// Written out rather than called from mbedtls so that the same code runs in the
// host test, where the crypto is the part most worth checking: a wrong key
// schedule produces packets that look perfectly well-formed and are silently
// undecodable by every other node. tools/mesh_test pins this against the
// FIPS-197 vector.
//
// CTR mode only ever encrypts, so the inverse cipher and its tables are absent.

const uint8_t kSbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

inline uint8_t xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0x00));
}

// Expanded key: 11 round keys for AES-128, 15 for AES-256.
struct AesKey {
    uint8_t rk[15 * 16];
    int rounds;
};

void aesExpandKey(const uint8_t* key, size_t keyLen, AesKey& out) {
    const int nk = (int)keyLen / 4;                 // 4 or 8 words
    out.rounds = nk + 6;                            // 10 or 14
    const int total = (out.rounds + 1) * 4;         // words in the schedule
    memcpy(out.rk, key, keyLen);
    uint8_t rcon = 1;
    for (int i = nk; i < total; ++i) {
        uint8_t t[4];
        memcpy(t, out.rk + (i - 1) * 4, 4);
        if (i % nk == 0) {
            const uint8_t r = t[0];
            t[0] = (uint8_t)(kSbox[t[1]] ^ rcon);
            t[1] = kSbox[t[2]];
            t[2] = kSbox[t[3]];
            t[3] = kSbox[r];
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            // AES-256 only: an extra SubWord with no rotate or rcon.
            for (int j = 0; j < 4; ++j) t[j] = kSbox[t[j]];
        }
        for (int j = 0; j < 4; ++j)
            out.rk[i * 4 + j] = (uint8_t)(out.rk[(i - nk) * 4 + j] ^ t[j]);
    }
}

void aesEncryptBlock(const AesKey& k, uint8_t s[16]) {
    for (int i = 0; i < 16; ++i) s[i] ^= k.rk[i];
    for (int round = 1; round <= k.rounds; ++round) {
        for (int i = 0; i < 16; ++i) s[i] = kSbox[s[i]];
        // ShiftRows — the state is column-major, so row r is bytes r, r+4, ....
        uint8_t t;
        t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
        t = s[2];  s[2]  = s[10]; s[10] = t;
        t = s[6];  s[6]  = s[14]; s[14] = t;
        t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
        if (round != k.rounds) {          // MixColumns is skipped in the last round
            for (int c = 0; c < 16; c += 4) {
                const uint8_t a0 = s[c], a1 = s[c + 1], a2 = s[c + 2], a3 = s[c + 3];
                const uint8_t all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                s[c + 0] = (uint8_t)(a0 ^ all ^ xtime((uint8_t)(a0 ^ a1)));
                s[c + 1] = (uint8_t)(a1 ^ all ^ xtime((uint8_t)(a1 ^ a2)));
                s[c + 2] = (uint8_t)(a2 ^ all ^ xtime((uint8_t)(a2 ^ a3)));
                s[c + 3] = (uint8_t)(a3 ^ all ^ xtime((uint8_t)(a3 ^ a0)));
            }
        }
        for (int i = 0; i < 16; ++i) s[i] ^= k.rk[round * 16 + i];
    }
}

// Meshtastic's default channel key. A stored PSK of a single byte N selects this
// key with the last byte advanced by N-1 (so "AQ==" — byte 1 — is it unchanged).
const uint8_t kDefaultPsk[16] = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
                                 0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};

}  // namespace

namespace mesh {

void encodeHeader(const Header& h, uint8_t* out) {
    put32(out + 0, h.dest);
    put32(out + 4, h.sender);
    put32(out + 8, h.id);
    uint8_t flags = (uint8_t)(h.hopLimit & FLAG_HOP_LIMIT);
    if (h.wantAck) flags |= FLAG_WANT_ACK;
    if (h.viaMqtt) flags |= FLAG_VIA_MQTT;
    flags |= (uint8_t)((h.hopStart << FLAG_HOP_START_SHIFT) & FLAG_HOP_START);
    out[12] = flags;
    out[13] = h.channelHash;
    out[14] = h.nextHop;
    out[15] = h.relayNode;
}

bool decodeHeader(const uint8_t* in, size_t len, Header& h) {
    if (len < HEADER_LEN) return false;
    h.dest = get32(in + 0);
    h.sender = get32(in + 4);
    h.id = get32(in + 8);
    const uint8_t flags = in[12];
    h.hopLimit = (uint8_t)(flags & FLAG_HOP_LIMIT);
    h.wantAck = (flags & FLAG_WANT_ACK) != 0;
    h.viaMqtt = (flags & FLAG_VIA_MQTT) != 0;
    h.hopStart = (uint8_t)((flags & FLAG_HOP_START) >> FLAG_HOP_START_SHIFT);
    h.channelHash = in[13];
    h.nextHop = in[14];
    h.relayNode = in[15];
    // A sender of 0 is not a node — it is a corrupt or truncated frame that
    // passed CRC, and letting it through would create a phantom neighbour.
    return h.sender != 0;
}

size_t encodeData(const Data& d, uint8_t* out, size_t cap) {
    Writer w{out, cap};
    w.u32(1, d.portnum);
    w.bytes(2, d.payload, d.payloadLen);
    w.boolean(3, d.wantResponse);
    w.u32(4, d.dest);
    w.u32(5, d.source);
    w.u32(6, d.requestId);
    w.u32(7, d.replyId);
    return w.overflow ? 0 : w.len;
}

bool decodeData(const uint8_t* in, size_t len, Data& d) {
    Reader r{in, len};
    while (!r.done()) {
        const uint64_t t = r.varint();
        if (r.bad) return false;
        const uint32_t field = (uint32_t)(t >> 3);
        const uint8_t wire = (uint8_t)(t & 7);
        switch (field) {
        case 1: d.portnum = (uint8_t)r.varint(); break;
        case 2: {
            const uint8_t* p;
            size_t n;
            if (!r.slice(p, n)) return false;
            if (n > MAX_PAYLOAD_LEN) n = MAX_PAYLOAD_LEN;
            memcpy(d.payload, p, n);
            d.payloadLen = n;
            break;
        }
        case 3: d.wantResponse = r.varint() != 0; break;
        case 4: d.dest = (uint32_t)r.varint(); break;
        case 5: d.source = (uint32_t)r.varint(); break;
        case 6: d.requestId = (uint32_t)r.varint(); break;
        case 7: d.replyId = (uint32_t)r.varint(); break;
        default: r.skip(wire); break;
        }
        if (r.bad) return false;
    }
    return true;
}

size_t encodeUser(const User& u, uint8_t* out, size_t cap) {
    Writer w{out, cap};
    w.str(1, u.id);
    w.str(2, u.longName);
    w.str(3, u.shortName);
    w.u32(5, u.hwModel);
    return w.overflow ? 0 : w.len;
}

bool decodeUser(const uint8_t* in, size_t len, User& u) {
    Reader r{in, len};
    while (!r.done()) {
        const uint64_t t = r.varint();
        if (r.bad) return false;
        const uint32_t field = (uint32_t)(t >> 3);
        const uint8_t wire = (uint8_t)(t & 7);
        switch (field) {
        case 1: r.copyStr(u.id, sizeof(u.id)); break;
        case 2: r.copyStr(u.longName, sizeof(u.longName)); break;
        case 3: r.copyStr(u.shortName, sizeof(u.shortName)); break;
        case 5: u.hwModel = (uint8_t)r.varint(); break;
        default: r.skip(wire); break;
        }
        if (r.bad) return false;
    }
    return true;
}

bool decodePosition(const uint8_t* in, size_t len, Position& p) {
    Reader r{in, len};
    bool haveLat = false, haveLon = false;
    while (!r.done()) {
        const uint64_t t = r.varint();
        if (r.bad) return false;
        const uint32_t field = (uint32_t)(t >> 3);
        const uint8_t wire = (uint8_t)(t & 7);
        switch (field) {
        case 1:                                   // latitude_i, 1e-7 degrees
            if (wire != 5) { r.skip(wire); break; }
            p.latitude = r.fixed32() / 1e7;
            haveLat = true;
            break;
        case 2:                                   // longitude_i
            if (wire != 5) { r.skip(wire); break; }
            p.longitude = r.fixed32() / 1e7;
            haveLon = true;
            break;
        case 3:                                   // altitude, metres MSL
            if (wire != 0) { r.skip(wire); break; }
            p.altitudeM = (int32_t)(uint32_t)r.varint();
            break;
        case 4:                                   // time the fix was taken
            if (wire != 5) { r.skip(wire); break; }
            p.utc = (uint32_t)r.fixed32();
            break;
        case 19:                                  // sats_in_view
            if (wire != 0) { r.skip(wire); break; }
            p.satsInView = (uint8_t)r.varint();
            break;
        case 23:                                  // precision_bits
            if (wire != 0) { r.skip(wire); break; }
            p.precisionBits = (uint8_t)r.varint();
            break;
        default:
            r.skip(wire);
            break;
        }
        if (r.bad) return false;
    }
    // A Position with no coordinates, or the null island, is a node telling us it
    // has no fix — not a node at 0,0 off the coast of Africa. Range-checked too,
    // because a garbled field that still passes CRC would otherwise plot.
    p.valid = haveLat && haveLon &&
              !(p.latitude == 0.0 && p.longitude == 0.0) &&
              p.latitude >= -90.0 && p.latitude <= 90.0 &&
              p.longitude >= -180.0 && p.longitude <= 180.0;
    return true;
}

size_t encodePosition(const Position& p, uint8_t* out, size_t cap) {
    Writer w{out, cap};
    // Coordinates are sfixed32 (wire type 5), not varints — the one place this
    // codec writes a fixed-width field.
    const int32_t latE7 = (int32_t)(p.latitude * 1e7);
    const int32_t lonE7 = (int32_t)(p.longitude * 1e7);
    w.tag(1, 5);
    w.raw((uint8_t)latE7); w.raw((uint8_t)(latE7 >> 8));
    w.raw((uint8_t)(latE7 >> 16)); w.raw((uint8_t)(latE7 >> 24));
    w.tag(2, 5);
    w.raw((uint8_t)lonE7); w.raw((uint8_t)(lonE7 >> 8));
    w.raw((uint8_t)(lonE7 >> 16)); w.raw((uint8_t)(lonE7 >> 24));
    if (p.altitudeM) {
        w.tag(3, 0);
        // int32 on the wire is a plain two's-complement varint, so a negative
        // altitude (below sea level, and Death Valley is a real place) sign-extends
        // to ten bytes rather than being zigzagged.
        w.varint((uint64_t)(int64_t)p.altitudeM);
    }
    if (p.utc) {
        w.tag(4, 5);
        w.raw((uint8_t)p.utc); w.raw((uint8_t)(p.utc >> 8));
        w.raw((uint8_t)(p.utc >> 16)); w.raw((uint8_t)(p.utc >> 24));
    }
    // location_source = 1 (LOC_INTERNAL): this came from our own receiver rather
    // than being typed in or inherited from a phone.
    w.u32(5, 1);
    w.u32(19, p.satsInView);
    return w.overflow ? 0 : w.len;
}

size_t encodeRouting(uint32_t errorReason, uint8_t* out, size_t cap) {
    // Routing is a oneof; error_reason is field 3. NONE (0) is the ACK, and
    // proto3 would normally omit a zero — but the field IS the message here, so
    // an empty Routing would carry no meaning at all. Written explicitly.
    Writer w{out, cap};
    w.tag(3, 0);
    w.varint(errorReason);
    return w.overflow ? 0 : w.len;
}

bool decodeRoutingError(const uint8_t* in, size_t len, uint32_t& err) {
    Reader r{in, len};
    bool found = false;
    while (!r.done()) {
        const uint64_t t = r.varint();
        if (r.bad) return false;
        const uint32_t field = (uint32_t)(t >> 3);
        const uint8_t wire = (uint8_t)(t & 7);
        if (field == 3 && wire == 0) {
            err = (uint32_t)r.varint();
            found = true;
        } else {
            r.skip(wire);
        }
        if (r.bad) return false;
    }
    // An empty Routing is a well-formed ACK: proto3 drops the zero on the wire.
    if (!found) err = 0;
    return true;
}

// Index 0 is LongFast because it is Meshtastic's default: an unconfigured node,
// and every node bought off a shelf, is on it.
const ModemPreset kPresets[PRESET_COUNT] = {
    {"LongFast",   11, 250.0f, 5},
    {"MediumSlow", 10, 250.0f, 5},
    {"MediumFast",  9, 250.0f, 5},
    {"ShortSlow",   8, 250.0f, 5},
    {"ShortFast",   7, 250.0f, 5},
    {"ShortTurbo",  7, 500.0f, 5},
};

int presetIndexByName(const char* name) {
    if (!name || !*name) return -1;
    for (int i = 0; i < PRESET_COUNT; ++i) {
        const char* a = kPresets[i].name;
        const char* b = name;
        while (*a && *b) {
            const char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            const char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
            if (ca != cb) break;
            ++a; ++b;
        }
        if (!*a && !*b) return i;
    }
    return -1;
}

const ModemPreset& preset(int index) {
    if (index < 0 || index >= PRESET_COUNT) return kPresets[0];
    return kPresets[index];
}

// meshtastic.ChannelSettings: psk = 2 (bytes), name = 3 (string). The other
// fields (channel_num, id, uplink/downlink, module settings) are not written —
// proto3 omits defaults, and a receiver fills them in itself.
size_t encodeChannelSettings(const ChannelSettings& c, uint8_t* out, size_t cap) {
    Writer w{out, cap};
    w.bytes(2, c.psk, c.pskLen);
    w.str(3, c.name);
    return w.overflow ? 0 : w.len;
}

bool decodeChannelSettings(const uint8_t* in, size_t len, ChannelSettings& c) {
    Reader r{in, len};
    while (!r.done()) {
        const uint64_t t = r.varint();
        if (r.bad) return false;
        const uint32_t field = (uint32_t)(t >> 3);
        const uint8_t wire = (uint8_t)(t & 7);
        switch (field) {
        case 2: {                                 // psk
            const uint8_t* p;
            size_t n;
            if (!r.slice(p, n)) return false;
            if (n > MAX_PSK_LEN) return false;    // not a key we could ever use
            memcpy(c.psk, p, n);
            c.pskLen = n;
            break;
        }
        case 3:                                   // name
            r.copyStr(c.name, sizeof(c.name));
            break;
        default:
            r.skip(wire);
            break;
        }
        if (r.bad) return false;
    }
    return true;
}

size_t encodeChannelSet(const ChannelSettings* chans, int n, uint8_t* out,
                        size_t cap) {
    Writer w{out, cap};
    for (int i = 0; i < n; ++i) {
        // Each entry is a length-delimited submessage, so it has to be encoded
        // before its length is known.
        uint8_t body[80];
        const size_t bn = encodeChannelSettings(chans[i], body, sizeof(body));
        if (bn == 0 && chans[i].name[0]) return 0;   // did not fit
        w.bytes(1, body, bn);
    }
    return w.overflow ? 0 : w.len;
}

bool decodeChannelSet(const uint8_t* in, size_t len, ChannelSettings* out, int max,
                      int& count) {
    count = 0;
    Reader r{in, len};
    while (!r.done()) {
        const uint64_t t = r.varint();
        if (r.bad) return false;
        const uint32_t field = (uint32_t)(t >> 3);
        const uint8_t wire = (uint8_t)(t & 7);
        if (field == 1 && wire == 2) {
            const uint8_t* p;
            size_t n;
            if (!r.slice(p, n)) return false;
            if (count < max) {
                out[count] = ChannelSettings();
                if (!decodeChannelSettings(p, n, out[count])) return false;
                count++;
            }
            // A set with more channels than we hold is not an error; the extras
            // are simply dropped, which is better than refusing the whole QR.
        } else {
            r.skip(wire);
        }
        if (r.bad) return false;
    }
    return true;
}

size_t expandPsk(const uint8_t* psk, size_t pskLen, uint8_t out[MAX_PSK_LEN]) {
    if (pskLen == 1) {
        defaultPsk(psk[0], out);
        return 16;
    }
    if (pskLen == 16 || pskLen == 32) {
        memcpy(out, psk, pskLen);
        return pskLen;
    }
    return 0;      // 0 = unencrypted, anything else is not a key we can use
}

uint8_t channelHash(const char* name, const uint8_t* psk, size_t pskLen) {
    uint8_t h = 0;
    for (const char* p = name; p && *p; ++p) h ^= (uint8_t)*p;
    for (size_t i = 0; i < pskLen; ++i) h ^= psk[i];
    return h;
}

uint32_t nameHash(const char* s) {
    uint32_t h = 5381;
    for (const char* p = s; p && *p; ++p)
        h = (uint32_t)(((h << 5) + h) + (uint8_t)*p);   // h * 33 + c
    return h;
}

uint32_t channelCount(float bwKhz, float freqStartMHz, float freqEndMHz,
                      float spacingMHz) {
    const float slot = spacingMHz + bwKhz / 1000.0f;
    if (slot <= 0.0f) return 1;
    const float n = std::floor((freqEndMHz - freqStartMHz) / slot);
    return n < 1.0f ? 1u : (uint32_t)n;
}

float channelFrequencyMHz(const char* channelName, float bwKhz,
                          float freqStartMHz, float freqEndMHz,
                          float spacingMHz) {
    const uint32_t n = channelCount(bwKhz, freqStartMHz, freqEndMHz, spacingMHz);
    const uint32_t slot = nameHash(channelName) % n;
    return freqStartMHz + bwKhz / 2000.0f + (float)slot * (bwKhz / 1000.0f);
}

void defaultPsk(uint8_t index, uint8_t out[16]) {
    memcpy(out, kDefaultPsk, 16);
    if (index > 1) out[15] = (uint8_t)(out[15] + (index - 1));
}

void ctrCrypt(const uint8_t* key, size_t keyLen, uint32_t fromNode,
              uint32_t packetId, uint32_t extraNonce, uint8_t* data,
              size_t len) {
    if (keyLen != 16 && keyLen != 32) return;
    AesKey k;
    aesExpandKey(key, keyLen, k);

    // The nonce IS the initial counter block. packetId occupies a 64-bit slot
    // even though it is a 32-bit value, so bytes 4..7 stay zero.
    uint8_t counter[16] = {};
    put32(counter + 0, packetId);
    put32(counter + 8, fromNode);
    put32(counter + 12, extraNonce);

    uint8_t stream[16];
    for (size_t off = 0; off < len; off += 16) {
        memcpy(stream, counter, 16);
        aesEncryptBlock(k, stream);
        const size_t n = len - off < 16 ? len - off : 16;
        for (size_t i = 0; i < n; ++i) data[off + i] ^= stream[i];
        // Big-endian increment of the whole block, as AES-CTR specifies (and as
        // mbedtls_aes_crypt_ctr, which the reference implementation uses, does).
        for (int i = 15; i >= 0; --i)
            if (++counter[i] != 0) break;
    }
}

void nodeIdString(uint32_t nodeNum, char* out, size_t cap) {
    if (cap < 11) {
        if (cap) out[0] = 0;
        return;
    }
    static const char* hex = "0123456789abcdef";
    out[0] = '!';
    for (int i = 0; i < 8; ++i)
        out[1 + i] = hex[(nodeNum >> (28 - 4 * i)) & 0xF];
    out[9] = 0;
}

}  // namespace mesh
