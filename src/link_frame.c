#include "link_frame.h"

#include <string.h>

void link_reasm_init(link_reasm_t* r, uint8_t* buf, size_t cap) {
    memset(r, 0, sizeof(*r));
    r->buf = buf;
    r->cap = cap;
}

void link_reasm_reset(link_reasm_t* r) {
    r->len = 0;
    r->in_msg = false;
    r->discard = false;
    r->synced = false;
    r->next = 0;
}

link_rx_t link_reasm_feed(link_reasm_t* r, const uint8_t* frag, size_t n, link_drop_t* why) {
    link_drop_t w = LINK_DROP_NONE;
    if (why) *why = LINK_DROP_NONE;
    if (n == 0 || !frag) {
        if (why) *why = LINK_DROP_EMPTY;
        return LINK_RX_IGNORED;
    }
    const uint8_t hdr = frag[0];
    const uint8_t ctr = hdr & LINK_HDR_COUNTER;

    // A gap loses whatever was open; this fragment is then judged on its own
    // (a FIRST starts clean).
    if (r->synced && ctr != r->next) {
        if (r->in_msg) {
            r->dropped++;
            w = LINK_DROP_GAP;
        }
        r->in_msg = false;
        r->discard = false;
    }
    r->synced = true;
    r->next = (uint8_t)((ctr + 1) & LINK_HDR_COUNTER);

    if (hdr & LINK_HDR_FIRST) {
        if (r->in_msg) {
            r->dropped++;
            w = LINK_DROP_FIRST_MID;
        }
        r->in_msg = true;
        r->discard = false;
        r->len = 0;
    } else if (!r->in_msg) {
        // The tail of an overflowed message is expected, not news.
        if (w == LINK_DROP_NONE && !r->discard) w = LINK_DROP_NO_FIRST;
        if (why) *why = w;
        return LINK_RX_IGNORED;
    }

    const size_t payload = n - 1;
    if (r->len + payload > r->cap) {
        r->dropped++;
        r->in_msg = false;
        r->discard = true;
        r->len = 0;
        if (why) *why = LINK_DROP_OVERFLOW;
        return LINK_RX_IGNORED;
    }
    if (payload) memcpy(r->buf + r->len, frag + 1, payload);
    r->len += payload;
    if (why) *why = w;
    if (hdr & LINK_HDR_LAST) {
        r->in_msg = false;
        return LINK_RX_COMPLETE;
    }
    return LINK_RX_PARTIAL;
}

const char* link_drop_name(link_drop_t why) {
    switch (why) {
    case LINK_DROP_NONE: return "none";
    case LINK_DROP_EMPTY: return "empty write";
    case LINK_DROP_GAP: return "counter gap";
    case LINK_DROP_FIRST_MID: return "FIRST inside a message";
    case LINK_DROP_NO_FIRST: return "continuation without FIRST";
    case LINK_DROP_OVERFLOW: return "message too large";
    }
    return "?";
}

size_t link_frag_build(const uint8_t* msg, size_t len, size_t off, uint8_t counter,
                       uint8_t* out, size_t max_frag, size_t* consumed) {
    if (max_frag < 2) max_frag = 2;
    size_t room = max_frag - 1;
    size_t left = off < len ? len - off : 0;
    size_t take = left < room ? left : room;
    uint8_t hdr = (uint8_t)(counter & LINK_HDR_COUNTER);
    if (off == 0) hdr |= LINK_HDR_FIRST;
    if (off + take >= len) hdr |= LINK_HDR_LAST;
    out[0] = hdr;
    if (take) memcpy(out + 1, msg + off, take);
    if (consumed) *consumed = take;
    return take + 1;
}

uint32_t link_crc32(uint32_t crc, const uint8_t* p, size_t n) {
    // Nibble table: 64 bytes of flash, ~2 lookups a byte — a 2 MB firmware
    // image hashes in tens of milliseconds.
    static const uint32_t k[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
        0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        crc = (crc >> 4) ^ k[crc & 0x0F];
        crc = (crc >> 4) ^ k[crc & 0x0F];
    }
    return ~crc;
}
