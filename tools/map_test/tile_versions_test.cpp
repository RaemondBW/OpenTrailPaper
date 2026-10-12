// Host tests for src/tile_versions.h: the on-card log of which version each
// tile and .poi was saved with (docs/prebuilt-tiles.md#versions).
//   tools/map_test/run_tile_versions.sh

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "tile_versions.h"

using namespace tile_versions;

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

static std::string roundTrip(const char* s) {
    uint64_t v;
    uint8_t t = parse(s, v);
    char out[VER_MAX];
    format(t, v, out, sizeof(out));
    return out;
}

static void append(std::vector<uint8_t>& log, const char* id, char kind, uint32_t size,
                   const char* version) {
    Rec r{};
    r.cell = cellFromId(id);
    r.kind = (uint8_t)kind;
    r.size = size;
    r.type = parse(version, r.value);
    uint8_t b[REC_LEN];
    encode(r, b);
    log.insert(log.end(), b, b + REC_LEN);
}

static std::string lookup(const std::vector<uint8_t>& log, const char* id, char kind,
                          uint32_t size) {
    std::vector<Rec> recs(log.size() / REC_LEN + 1);
    int n = latest(recs.data(), decodeAll(log.data(), log.size(), recs.data()));
    const Rec* r = find(recs.data(), n, cellFromId(id), (uint8_t)kind);
    if (!r || r->size != size) return "";
    char out[VER_MAX];
    format(r->type, r->value, out, sizeof(out));
    return out;
}

int main() {
    // Records survive the trip through the binary form exactly as the app sent
    // them; anything the device cannot represent is dropped (app falls back).
    CHECK(roundTrip("cdn:0123456789abcdef") == "cdn:0123456789abcdef");
    CHECK(roundTrip("cdn:ffffffffffffffff") == "cdn:ffffffffffffffff");
    CHECK(roundTrip("cdn:") == "cdn:");
    CHECK(roundTrip("phone:1760000000") == "phone:1760000000");
    CHECK(roundTrip("phone:0") == "phone:0");
    CHECK(roundTrip("cdn:0123") == "");
    CHECK(roundTrip("cdn:0123456789ABCDEF") == "");
    CHECK(roundTrip("cdn:0123456789abcdeg") == "");
    CHECK(roundTrip("phone:") == "");
    CHECK(roundTrip("phone:12x") == "");
    CHECK(roundTrip("other:1") == "");
    CHECK(roundTrip("") == "");
    CHECK(roundTrip(nullptr) == "");

    CHECK(cellFromId("861f8d8afffffff") == 0x861f8d8afffffffULL);
    CHECK(cellFromId("861f8d8afffffff.poi") == 0x861f8d8afffffffULL);
    CHECK(cellFromId("tile") == 0);
    CHECK(cellFromId("") == 0);

    // Encode / decode, and the check byte catching any changed byte.
    {
        Rec r{0x861f8d8afffffffULL, 0x0123456789abcdefULL, 12345, 'e', CDN};
        uint8_t b[REC_LEN];
        encode(r, b);
        Rec d{};
        CHECK(decode(b, d));
        CHECK(d.cell == r.cell && d.value == r.value && d.size == r.size &&
              d.kind == r.kind && d.type == r.type);
        for (size_t i = 0; i < REC_LEN; ++i) {
            uint8_t c[REC_LEN];
            memcpy(c, b, REC_LEN);
            c[i] ^= 0x10;
            CHECK(!decode(c, d));
        }
        uint8_t zero[REC_LEN] = {};
        CHECK(!decode(zero, d));
        uint8_t ff[REC_LEN];
        memset(ff, 0xFF, sizeof(ff));
        CHECK(!decode(ff, d));
    }

    // Last record wins per (cell, kind); kinds are independent; the size must
    // match; CLEARED forgets.
    {
        std::vector<uint8_t> log;
        append(log, "861f8d8afffffff", 'e', 100, "phone:1700000000");
        append(log, "861f8d8afffffff", 'p', 50, "cdn:");
        append(log, "861f8d8b7ffffff", 'e', 200, "cdn:aaaaaaaaaaaaaaaa");
        append(log, "861f8d8afffffff", 'e', 120, "cdn:0123456789abcdef");
        CHECK(lookup(log, "861f8d8afffffff", 'e', 120) == "cdn:0123456789abcdef");
        CHECK(lookup(log, "861f8d8afffffff", 'e', 100) == "");   // replaced since
        CHECK(lookup(log, "861f8d8afffffff", 'p', 50) == "cdn:");
        CHECK(lookup(log, "861f8d8b7ffffff", 'e', 200) == "cdn:aaaaaaaaaaaaaaaa");
        CHECK(lookup(log, "861f8d8b7ffffff", 'p', 200) == "");
        CHECK(lookup(log, "861f8d8a7ffffff", 'e', 200) == "");   // never recorded

        append(log, "861f8d8b7ffffff", 'e', 210, nullptr);       // older app re-sent it
        CHECK(lookup(log, "861f8d8b7ffffff", 'e', 210) == "");
        CHECK(lookup(log, "861f8d8b7ffffff", 'e', 200) == "");
        append(log, "861f8d8b7ffffff", 'e', 220, "phone:1760000000");
        CHECK(lookup(log, "861f8d8b7ffffff", 'e', 220) == "phone:1760000000");

        // A torn last record (power cut mid-append) is ignored; the rest stand.
        std::vector<uint8_t> torn = log;
        append(torn, "861f8d8afffffff", 'e', 130, "cdn:1111111111111111");
        torn.resize(torn.size() - 7);
        CHECK(lookup(torn, "861f8d8afffffff", 'e', 120) == "cdn:0123456789abcdef");
        // Garbage in the middle (a cluster that never got written) costs only
        // the records it covers.
        std::vector<uint8_t> holed = log;
        memset(holed.data() + REC_LEN * 2, 0, REC_LEN);           // the 861f8d8b7 'e' 200 one
        CHECK(lookup(holed, "861f8d8afffffff", 'e', 120) == "cdn:0123456789abcdef");
        CHECK(lookup(holed, "861f8d8b7ffffff", 'e', 220) == "phone:1760000000");
    }

    // Compaction: re-sending the same hexes over and over is what grows the
    // log; latest() must leave one record per live file and compaction must
    // kick in once superseded ones dominate.
    {
        std::vector<uint8_t> log;
        char id[24];
        for (int round = 0; round < 5; ++round)
            for (int i = 0; i < 300; ++i) {
                snprintf(id, sizeof(id), "861f8d%09x", 0x8afffffu + (unsigned)i * 0x10u);
                append(log, id, 'e', 1000 + i, round == 4 ? "cdn:0123456789abcdef"
                                                          : "phone:1700000000");
            }
        int total = (int)(log.size() / REC_LEN);
        std::vector<Rec> recs(total);
        int live = latest(recs.data(), decodeAll(log.data(), log.size(), recs.data()));
        CHECK(total == 1500);
        CHECK(live == 300);
        CHECK(wantsCompaction(total, live));
        CHECK(!wantsCompaction(300, 300));
        CHECK(!wantsCompaction(600, 300));
        for (int i = 0; i < live; ++i) CHECK(recs[i].type == CDN);
        // Re-encoding the compacted set reads back the same.
        std::vector<uint8_t> out;
        for (int i = 0; i < live; ++i) {
            uint8_t b[REC_LEN];
            encode(recs[i], b);
            out.insert(out.end(), b, b + REC_LEN);
        }
        snprintf(id, sizeof(id), "861f8d%09x", 0x8afffffu + 7u * 0x10u);
        CHECK(lookup(out, id, 'e', 1007) == "cdn:0123456789abcdef");
    }

    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("tile_versions: all tests passed\n");
    return 0;
}
