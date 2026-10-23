// Host tests for the phone link (docs/ble-link.md): framing and reassembly
// (src/link_frame.c), CRC-32, and a nanopb round trip of the generated
// messages the firmware sends and receives. Run tools/link_test/run_link_test.sh.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link_frame.h"
#include "link.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

static int failures = 0;
#define CHECK(c)                                                             \
    do {                                                                     \
        if (!(c)) {                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            failures++;                                                      \
        }                                                                    \
    } while (0)

// Sender side: split msg into fragments of at most max_frag bytes, starting
// at counter *ctr; returns the fragment count and the fragments in frags.
static int fragment(const uint8_t* msg, size_t len, size_t max_frag, uint8_t* ctr,
                    uint8_t frags[][600], size_t* flen) {
    int n = 0;
    size_t off = 0;
    do {
        size_t used = 0;
        flen[n] = link_frag_build(msg, len, off, *ctr, frags[n], max_frag, &used);
        off += used;
        *ctr = (uint8_t)((*ctr + 1) & 0x0F);
        n++;
    } while (off < len);
    return n;
}

static uint8_t frags[64][600];
static size_t flen[64];

static void test_roundtrip_sizes(void) {
    static uint8_t rxbuf[LINK_MAX_MESSAGE];
    link_reasm_t r;
    link_reasm_init(&r, rxbuf, sizeof(rxbuf));
    uint8_t ctr = 0;
    uint8_t msg[3000];
    for (size_t i = 0; i < sizeof(msg); ++i) msg[i] = (uint8_t)(i * 7 + 3);
    const size_t lens[] = {0, 1, 19, 20, 243, 244, 1000, 3000};
    const size_t mtus[] = {2, 20, 244, 512};
    for (size_t li = 0; li < sizeof(lens) / sizeof(lens[0]); ++li) {
        for (size_t mi = 0; mi < sizeof(mtus) / sizeof(mtus[0]); ++mi) {
            if (lens[li] / (mtus[mi] - 1) + 1 > 64) continue;   // past the scratch
            int n = fragment(msg, lens[li], mtus[mi], &ctr, frags, flen);
            link_rx_t res = LINK_RX_PARTIAL;
            for (int i = 0; i < n; ++i) {
                CHECK(flen[i] <= mtus[mi]);
                CHECK(((frags[i][0] & LINK_HDR_FIRST) != 0) == (i == 0));
                CHECK(((frags[i][0] & LINK_HDR_LAST) != 0) == (i == n - 1));
                CHECK((frags[i][0] & 0x30) == 0);
                link_drop_t why;
                res = link_reasm_feed(&r, frags[i], flen[i], &why);
                CHECK(why == LINK_DROP_NONE);
                CHECK(res == (i == n - 1 ? LINK_RX_COMPLETE : LINK_RX_PARTIAL));
            }
            CHECK(res == LINK_RX_COMPLETE);
            CHECK(r.len == lens[li]);
            CHECK(memcmp(r.buf, msg, lens[li]) == 0);
        }
    }
    CHECK(r.dropped == 0);
}

static void test_single_fragment_header(void) {
    uint8_t out[8];
    size_t used;
    const uint8_t m[3] = {1, 2, 3};
    size_t n = link_frag_build(m, 3, 0, 5, out, sizeof(out), &used);
    CHECK(n == 4 && used == 3);
    CHECK(out[0] == (0xC0 | 5));
    n = link_frag_build(m, 0, 0, 17, out, sizeof(out), &used);   // counter masks to 1
    CHECK(n == 1 && used == 0 && out[0] == 0xC1);
}

static void test_errors(void) {
    static uint8_t rxbuf[64];
    link_reasm_t r;
    link_reasm_init(&r, rxbuf, sizeof(rxbuf));
    link_drop_t why;

    // Continuation with nothing open.
    const uint8_t cont[] = {0x00, 1, 2};
    CHECK(link_reasm_feed(&r, cont, sizeof(cont), &why) == LINK_RX_IGNORED);
    CHECK(why == LINK_DROP_NO_FIRST);

    // FIRST (ctr 1), then a gap (ctr 3 instead of 2) loses it; a FIRST|LAST
    // after the gap still completes.
    const uint8_t first[] = {0x81, 'a', 'b'};
    CHECK(link_reasm_feed(&r, first, sizeof(first), &why) == LINK_RX_PARTIAL);
    const uint8_t gapped[] = {0x03, 'c'};
    CHECK(link_reasm_feed(&r, gapped, sizeof(gapped), &why) == LINK_RX_IGNORED);
    CHECK(why == LINK_DROP_GAP);
    CHECK(r.dropped == 1);
    const uint8_t whole[] = {0xC4, 'x', 'y'};
    CHECK(link_reasm_feed(&r, whole, sizeof(whole), &why) == LINK_RX_COMPLETE);
    CHECK(why == LINK_DROP_NONE && r.len == 2 && memcmp(r.buf, "xy", 2) == 0);

    // A gap straight into a FIRST|LAST: the open message is lost, the new one
    // is complete.
    const uint8_t open[] = {0x85, 'q'};
    CHECK(link_reasm_feed(&r, open, sizeof(open), &why) == LINK_RX_PARTIAL);
    const uint8_t jump[] = {0xC9, 'z'};
    CHECK(link_reasm_feed(&r, jump, sizeof(jump), &why) == LINK_RX_COMPLETE);
    CHECK(why == LINK_DROP_GAP && r.len == 1 && r.buf[0] == 'z');

    // FIRST inside an open message.
    const uint8_t f1[] = {0x8A, '1'};
    const uint8_t f2[] = {0xCB, '2'};
    CHECK(link_reasm_feed(&r, f1, sizeof(f1), &why) == LINK_RX_PARTIAL);
    CHECK(link_reasm_feed(&r, f2, sizeof(f2), &why) == LINK_RX_COMPLETE);
    CHECK(why == LINK_DROP_FIRST_MID && r.len == 1 && r.buf[0] == '2');

    // Overflow: 64-byte buffer, 3 x 30-byte fragments. The tail is ignored
    // quietly and the next FIRST works.
    uint8_t big[31];
    memset(big, 'o', sizeof(big));
    big[0] = 0x8C;
    CHECK(link_reasm_feed(&r, big, sizeof(big), &why) == LINK_RX_PARTIAL);
    big[0] = 0x0D;
    CHECK(link_reasm_feed(&r, big, sizeof(big), &why) == LINK_RX_PARTIAL);
    big[0] = 0x0E;
    CHECK(link_reasm_feed(&r, big, sizeof(big), &why) == LINK_RX_IGNORED);
    CHECK(why == LINK_DROP_OVERFLOW);
    big[0] = 0x4F;   // the LAST of the overflowed message
    CHECK(link_reasm_feed(&r, big, sizeof(big), &why) == LINK_RX_IGNORED);
    CHECK(why == LINK_DROP_NONE);
    const uint8_t after[] = {0xC0, 'k'};   // counter wrapped 15 -> 0
    CHECK(link_reasm_feed(&r, after, sizeof(after), &why) == LINK_RX_COMPLETE);
    CHECK(why == LINK_DROP_NONE && r.buf[0] == 'k');

    // Empty write.
    CHECK(link_reasm_feed(&r, NULL, 0, &why) == LINK_RX_IGNORED && why == LINK_DROP_EMPTY);

    // Reset forgets the counter: any counter syncs again.
    link_reasm_reset(&r);
    const uint8_t any[] = {0xC7, 'r'};
    CHECK(link_reasm_feed(&r, any, sizeof(any), &why) == LINK_RX_COMPLETE && why == LINK_DROP_NONE);
}

static void test_crc(void) {
    const char* s = "123456789";
    CHECK(link_crc32(0, (const uint8_t*)s, 9) == 0xCBF43926u);
    uint32_t c = link_crc32(0, (const uint8_t*)s, 4);
    c = link_crc32(c, (const uint8_t*)s + 4, 5);
    CHECK(c == 0xCBF43926u);
    CHECK(link_crc32(0, NULL, 0) == 0);
}

static size_t encode_device(const otp_link_DeviceMsg* m, uint8_t* buf, size_t cap) {
    pb_ostream_t os = pb_ostream_from_buffer(buf, cap);
    if (!pb_encode(&os, otp_link_DeviceMsg_fields, m)) {
        fprintf(stderr, "encode: %s\n", PB_GET_ERROR(&os));
        return 0;
    }
    return os.bytes_written;
}

static void test_nanopb(void) {
    static otp_link_PhoneMsg pm, pm2;
    static otp_link_DeviceMsg dm, dm2;
    static uint8_t buf[otp_link_DeviceMsg_size + 16];

    // Phone -> device: Hello through the framing at MTU 23 (20-byte fragments).
    memset(&pm, 0, sizeof(pm));
    pm.req = 7;
    pm.which_body = otp_link_PhoneMsg_hello_tag;
    pm.body.hello.proto = otp_link_ProtoVersion_PROTO_VERSION_CURRENT;
    pm.body.hello.platform = otp_link_Platform_PLATFORM_IOS;
    strcpy(pm.body.hello.app_version, "2.4 (61)");
    pm.body.hello.features_count = 1;
    pm.body.hello.features[0] = otp_link_Feature_FEATURE_MEDIA_COMMAND_HANDLER;
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof(buf));
    CHECK(pb_encode(&os, otp_link_PhoneMsg_fields, &pm));
    uint8_t ctr = 9;
    int n = fragment(buf, os.bytes_written, 20, &ctr, frags, flen);
    static uint8_t rxbuf[LINK_MAX_MESSAGE];
    link_reasm_t r;
    link_reasm_init(&r, rxbuf, sizeof(rxbuf));
    link_rx_t res = LINK_RX_IGNORED;
    for (int i = 0; i < n; ++i) res = link_reasm_feed(&r, frags[i], flen[i], NULL);
    CHECK(res == LINK_RX_COMPLETE);
    pb_istream_t is = pb_istream_from_buffer(r.buf, r.len);
    CHECK(pb_decode(&is, otp_link_PhoneMsg_fields, &pm2));
    CHECK(pm2.req == 7 && pm2.which_body == otp_link_PhoneMsg_hello_tag);
    CHECK(strcmp(pm2.body.hello.app_version, "2.4 (61)") == 0);
    CHECK(pm2.body.hello.features_count == 1 &&
          pm2.body.hello.features[0] == otp_link_Feature_FEATURE_MEDIA_COMMAND_HANDLER);

    // Optional fields: present vs absent survive the trip (Settings partial set).
    memset(&pm, 0, sizeof(pm));
    pm.req = 8;
    pm.which_body = otp_link_PhoneMsg_set_settings_tag;
    pm.body.set_settings.has_wheel_mm = true;
    pm.body.set_settings.wheel_mm = 2105;
    pm.body.set_settings.has_use_miles = true;
    pm.body.set_settings.use_miles = false;   // present AND false
    os = pb_ostream_from_buffer(buf, sizeof(buf));
    CHECK(pb_encode(&os, otp_link_PhoneMsg_fields, &pm));
    is = pb_istream_from_buffer(buf, os.bytes_written);
    CHECK(pb_decode(&is, otp_link_PhoneMsg_fields, &pm2));
    CHECK(pm2.body.set_settings.has_wheel_mm && pm2.body.set_settings.wheel_mm == 2105);
    CHECK(pm2.body.set_settings.has_use_miles && !pm2.body.set_settings.use_miles);
    CHECK(!pm2.body.set_settings.has_ftp_w && !pm2.body.set_settings.has_tz_min);

    // A string past its nanopb limit fails to decode rather than overflowing.
    uint8_t bad[80];
    size_t bl = 0;
    bad[bl++] = 0x12;                  // field 2 (hello), length-delimited
    bad[bl++] = 2 + 40;
    bad[bl++] = 0x1A;                  // Hello.app_version
    bad[bl++] = 40;                    // > 31 + NUL
    memset(bad + bl, 'v', 40);
    bl += 40;
    is = pb_istream_from_buffer(bad, bl);
    CHECK(!pb_decode(&is, otp_link_PhoneMsg_fields, &pm2));

    // An unknown body field is skipped: which_body stays 0 (-> UNSUPPORTED).
    const uint8_t unknown[] = {0x08, 0x05, 0xFA, 0x0F, 0x00};   // req 5, field 250 empty
    is = pb_istream_from_buffer(unknown, sizeof(unknown));
    CHECK(pb_decode(&is, otp_link_PhoneMsg_fields, &pm2));
    CHECK(pm2.req == 5 && pm2.which_body == 0);

    // Device -> phone: a full LiveFrame stays under the advertised maximum.
    memset(&dm, 0, sizeof(dm));
    dm.which_body = otp_link_DeviceMsg_live_tag;
    otp_link_LiveFrame* f = &dm.body.live;
    f->seq = 0xFFFFFFFFu;
    f->has_ride = true;
    f->ride.has_speed_dkmh = true;
    f->ride.speed_dkmh = 0;            // present zero, not "no reading"
    f->ride.has_grade_dpct = true;
    f->ride.grade_dpct = -125;
    f->ride.distance_m = 123456;
    f->has_stopwatch = true;
    f->stopwatch.recent_count = 10;
    for (int i = 0; i < 10; ++i) {
        f->stopwatch.recent[i].number = 99 - i;
        f->stopwatch.recent[i].split_ms = 0xFFFFFFF0u;
        f->stopwatch.recent[i].has_speed_dkmh = true;
        f->stopwatch.recent[i].speed_dkmh = 65535;
    }
    f->has_radar = true;
    f->radar.distances_m_count = 8;
    f->has_position = true;
    f->position.lat_e7 = -900000000;
    f->position.lon_e7 = -1800000000;
    size_t len = encode_device(&dm, buf, sizeof(buf));
    CHECK(len > 0 && len <= otp_link_DeviceMsg_size);
    is = pb_istream_from_buffer(buf, len);
    CHECK(pb_decode(&is, otp_link_DeviceMsg_fields, &dm2));
    CHECK(dm2.body.live.ride.has_speed_dkmh && dm2.body.live.ride.speed_dkmh == 0);
    CHECK(!dm2.body.live.ride.has_power_w);
    CHECK(dm2.body.live.ride.grade_dpct == -125);
    CHECK(dm2.body.live.stopwatch.recent_count == 10);
    CHECK(dm2.body.live.position.lon_e7 == -1800000000);

    // A bulk Chunk sized by the doc's formula (MTU - 3 - 1 - 16) fits one
    // fragment at MTU 247 for a counter-style xfer id and offsets below
    // 256 MB. (Worst-case varints need 18; the firmware sizes exactly.)
    memset(&dm, 0, sizeof(dm));
    dm.which_body = otp_link_DeviceMsg_chunk_tag;
    dm.body.chunk.xfer = 127;
    dm.body.chunk.offset = (1u << 28) - 1;
    dm.body.chunk.data.size = 247 - 3 - 1 - 16;
    len = encode_device(&dm, buf, sizeof(buf));
    CHECK(len > 0 && len <= 247 - 3 - 1);

    // The largest device messages encode inside DeviceMsg_size.
    memset(&dm, 0, sizeof(dm));
    dm.req = 0xFFFFFFFFu;
    dm.which_body = otp_link_DeviceMsg_dash_tag;
    memset(dm.body.dash.text, 'x', sizeof(dm.body.dash.text) - 1);
    len = encode_device(&dm, buf, sizeof(buf));
    CHECK(len > 0 && len <= otp_link_DeviceMsg_size);
    CHECK(otp_link_PhoneMsg_size <= LINK_MAX_MESSAGE);
}

int main(void) {
    test_roundtrip_sizes();
    test_single_fragment_header();
    test_errors();
    test_crc();
    test_nanopb();
    if (failures) {
        fprintf(stderr, "link_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("link_test: all passed\n");
    return 0;
}
