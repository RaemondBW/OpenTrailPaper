#pragma once

// Phone link framing (docs/ble-link.md "Framing"): one protobuf message split
// into ATT-sized fragments, each with a 1-byte header
//   b7 FIRST, b6 LAST, b5-b4 reserved (0), b3-b0 fragment counter
// The counter runs 0-15 per direction across all fragments, wrapping.
//
// Plain C with no Arduino dependency, so tools/link_test builds it on the host.
// Not thread-safe: one reassembler per receiving task, one sender per side.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINK_HDR_FIRST 0x80u
#define LINK_HDR_LAST 0x40u
#define LINK_HDR_COUNTER 0x0Fu

// Largest reassembled message the device accepts (DeviceHello.max_message).
#define LINK_MAX_MESSAGE 4096u

// Why a fragment (or the partial message before it) was dropped.
typedef enum {
    LINK_DROP_NONE = 0,
    LINK_DROP_EMPTY,       // zero-length write
    LINK_DROP_GAP,         // counter skipped: the partial message is gone
    LINK_DROP_FIRST_MID,   // FIRST while a message was open: the old one is gone
    LINK_DROP_NO_FIRST,    // continuation with no open message
    LINK_DROP_OVERFLOW,    // message larger than the buffer
} link_drop_t;

typedef enum {
    LINK_RX_PARTIAL = 0,   // fragment taken, message not complete yet
    LINK_RX_COMPLETE,      // buf[0..len) holds a whole message
    LINK_RX_IGNORED,       // fragment dropped (see link_drop_t)
} link_rx_t;

typedef struct {
    uint8_t* buf;
    size_t cap;
    size_t len;
    bool in_msg;     // a FIRST arrived and its LAST has not
    bool discard;    // overflowed: ignore continuations until the next FIRST
    bool synced;     // a counter has been seen (the first fragment syncs)
    uint8_t next;    // expected counter
    uint32_t dropped;   // messages lost to gaps / overflow / interleaving
} link_reasm_t;

void link_reasm_init(link_reasm_t* r, uint8_t* buf, size_t cap);
// Forget any partial message and the counter (a new connection).
void link_reasm_reset(link_reasm_t* r);
// Feed one fragment. On LINK_RX_COMPLETE the message is r->buf[0..r->len),
// valid until the next call. *why (optional) reports a drop, which can come
// with any result: a gap that lost a partial message, then a FIRST|LAST
// fragment that completes a new one, returns COMPLETE with why = GAP.
link_rx_t link_reasm_feed(link_reasm_t* r, const uint8_t* frag, size_t n, link_drop_t* why);

const char* link_drop_name(link_drop_t why);

// Build the fragment of msg[0..len) that starts at `off` into out (at most
// max_frag bytes, header included; max_frag >= 2). Returns the fragment
// length and sets *consumed to the payload bytes it carries. The caller
// sends it, then advances off by *consumed and the counter by one, until off
// reaches len. A zero-length message is one header-only FIRST|LAST fragment.
size_t link_frag_build(const uint8_t* msg, size_t len, size_t off, uint8_t counter,
                       uint8_t* out, size_t max_frag, size_t* consumed);

// IEEE CRC-32 as zlib's crc32(): start with 0, feed pieces in order.
uint32_t link_crc32(uint32_t crc, const uint8_t* p, size_t n);

#ifdef __cplusplus
}
#endif
