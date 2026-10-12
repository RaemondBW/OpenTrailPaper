#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Which version of each H3 tile (.ebm) and .poi is on the card, as the phone
// that sent it said: "cdn:<16 hex>" for a file from the pre-built CDN set (the
// first 16 hex of its sha256, tools/tiles/build_region.mjs), "cdn:" for the
// synthesised empty .poi of a CDN hex with no POIs, "phone:<secs>" for one
// built on the phone from Overpass (docs/prebuilt-tiles.md#versions). The
// device keeps these so ANY phone can tell a current hex from an update, not
// only the one that sent it.
//
// Stored as one append-only log of fixed 24-byte records, /maps/tiles.ver,
// beside the tile tree rather than in it (so the tile walk never sees it):
//   [0] 'V'  [1] kind 'e'|'p'  [2] type  [3] check  [4..7] file size LE
//   [8..15] cell (the id as a u64)  [16..23] value LE
// type 1 cdn (value = the 16 hex digits), 2 cdn-empty, 3 phone (value = secs),
// 0 cleared (the file was replaced by a sender that gave no version).
//
// Why a log: a save appends 24 bytes (one open of a file in /maps, which holds
// a handful of entries), never rewrites anything, so a power cut can at worst
// leave a torn last record — which the check byte and the fixed size both
// reject. Reading it is one sequential file, not a lookup per tile. The last
// record for a (cell, kind) wins; the log is compacted when it is mostly
// superseded records.
//
// Why the file size: the card can change behind the device's back (USB mass
// storage, the website ZIP). A record only counts while the file on the card
// still has the size it was saved with — a cheap check, since the directory
// walk that lists the tiles has every size for free.
//
// Pure C++ with no Arduino dependency so tools/map_test can test it on the host.

namespace tile_versions {

constexpr size_t REC_LEN = 24;
constexpr size_t VER_MAX = 24;   // longest version string, NUL included

enum Type : uint8_t { CLEARED = 0, CDN = 1, CDN_EMPTY = 2, PHONE = 3 };

struct Rec {
    uint64_t cell;
    uint64_t value;
    uint32_t size;
    uint8_t kind;   // 'e' or 'p'
    uint8_t type;
};

// The id as a u64 (an H3 id is its index in hex). 0 for anything that is not
// 1..16 hex digits, optionally followed by an extension.
inline uint64_t cellFromId(const char* id) {
    uint64_t v = 0;
    int n = 0;
    for (; id[n] && id[n] != '.'; ++n) {
        char c = id[n];
        int d = (c >= '0' && c <= '9') ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0 || n >= 16) return 0;
        v = (v << 4) | (uint64_t)d;
    }
    return n ? v : 0;
}

// "cdn:<16 hex>" / "cdn:" / "phone:<secs>" -> type + value. Anything else
// (including null or empty) is CLEARED: the device cannot vouch for it, so
// the app falls back to its own record.
inline uint8_t parse(const char* s, uint64_t& value) {
    value = 0;
    if (!s) return CLEARED;
    if (strncmp(s, "cdn:", 4) == 0) {
        const char* h = s + 4;
        if (!*h) return CDN_EMPTY;
        if (strlen(h) != 16) return CLEARED;
        for (int i = 0; i < 16; ++i) {
            char c = h[i];
            int d = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;   // lowercase, as built
            if (d < 0) return CLEARED;
            value = (value << 4) | (uint64_t)d;
        }
        return CDN;
    }
    if (strncmp(s, "phone:", 6) == 0) {
        const char* d = s + 6;
        if (!*d || strlen(d) > 12) return CLEARED;
        for (; *d; ++d) {
            if (*d < '0' || *d > '9') return CLEARED;
            value = value * 10 + (uint64_t)(*d - '0');
        }
        return PHONE;
    }
    return CLEARED;
}

// The string the phone sent, back. Empty for CLEARED.
inline void format(uint8_t type, uint64_t value, char* out, size_t len) {
    if (!len) return;
    switch (type) {
    case CDN:       snprintf(out, len, "cdn:%016llx", (unsigned long long)value); break;
    case CDN_EMPTY: snprintf(out, len, "cdn:"); break;
    case PHONE:     snprintf(out, len, "phone:%llu", (unsigned long long)value); break;
    default:        out[0] = 0; break;
    }
}

inline uint8_t check(const uint8_t* b) {
    uint8_t c = 0x5A;
    for (size_t i = 0; i < REC_LEN; ++i)
        if (i != 3) c = (uint8_t)((c << 1 | c >> 7) ^ b[i]);
    return c;
}

inline void encode(const Rec& r, uint8_t* b) {
    b[0] = 'V';
    b[1] = r.kind;
    b[2] = r.type;
    b[3] = 0;
    for (int i = 0; i < 4; ++i) b[4 + i] = (uint8_t)(r.size >> (8 * i));
    for (int i = 0; i < 8; ++i) b[8 + i] = (uint8_t)(r.cell >> (8 * i));
    for (int i = 0; i < 8; ++i) b[16 + i] = (uint8_t)(r.value >> (8 * i));
    b[3] = check(b);
}

// False for a torn or garbled record.
inline bool decode(const uint8_t* b, Rec& r) {
    if (b[0] != 'V' || (b[1] != 'e' && b[1] != 'p') || b[2] > PHONE || b[3] != check(b))
        return false;
    r.kind = b[1];
    r.type = b[2];
    r.size = 0;
    for (int i = 0; i < 4; ++i) r.size |= (uint32_t)b[4 + i] << (8 * i);
    r.cell = 0;
    for (int i = 0; i < 8; ++i) r.cell |= (uint64_t)b[8 + i] << (8 * i);
    r.value = 0;
    for (int i = 0; i < 8; ++i) r.value |= (uint64_t)b[16 + i] << (8 * i);
    return r.cell != 0;
}

inline bool keyLess(const Rec& a, const Rec& b) {
    return a.cell != b.cell ? a.cell < b.cell : a.kind < b.kind;
}

// Decode a log image (`len` bytes) into `out` (room for len / REC_LEN), in
// file order, skipping bad records. Returns the count.
inline int decodeAll(const uint8_t* log, size_t len, Rec* out) {
    int n = 0;
    for (size_t off = 0; off + REC_LEN <= len; off += REC_LEN)
        if (decode(log + off, out[n])) ++n;
    return n;
}

// Sort by (cell, kind) and keep only the last record of each in file order,
// dropping those whose last word is CLEARED. Returns the new count; the
// records stay sorted for find().
inline int latest(Rec* recs, int n) {
    std::stable_sort(recs, recs + n, keyLess);   // stable: file order within a key
    int w = 0;
    for (int i = 0; i < n; ++i) {
        if (i + 1 < n && !keyLess(recs[i], recs[i + 1])) continue;   // superseded
        if (recs[i].type != CLEARED) recs[w++] = recs[i];
    }
    return w;
}

// The record for (cell, kind) in the output of latest(), or null.
inline const Rec* find(const Rec* recs, int n, uint64_t cell, uint8_t kind) {
    Rec key{cell, 0, 0, kind, 0};
    const Rec* it = std::lower_bound(recs, recs + n, key, keyLess);
    return (it != recs + n && it->cell == cell && it->kind == kind) ? it : nullptr;
}

// Compact when superseded records outnumber live ones (plus some slack, so a
// small log is never rewritten).
inline bool wantsCompaction(int total, int live) {
    return total > 2 * live + 256;
}

}  // namespace tile_versions
