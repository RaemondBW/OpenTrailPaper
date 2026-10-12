#include "mesh_crypto.h"

#include <cstring>

namespace {

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)
// ---------------------------------------------------------------------------

const uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void shaBlock(uint32_t h[8], const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6],
             hh = h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) +
                            ((e & f) ^ (~e & g)) + kK[i] + w[i];
        const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) +
                            ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

// ---------------------------------------------------------------------------
// X25519 — TweetNaCl's crypto_scalarmult, public domain
// ---------------------------------------------------------------------------
//
// A field element is 16 limbs of 16 bits held in int64 so products and carries
// never overflow. Left shifts of possibly-negative values are written as
// multiplications to stay clear of undefined behaviour.

typedef int64_t gf[16];

const gf k121665 = {0xDB41, 1};

void car25519(gf o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += (int64_t)1 << 16;
        const int64_t c = o[i] >> 16;
        if (i < 15) o[i + 1] += c - 1;
        else        o[0] += 38 * (c - 1);
        o[i] -= c * 65536;
    }
}

void sel25519(gf p, gf q, int b) {
    const int64_t c = ~(int64_t)(b - 1);
    for (int i = 0; i < 16; ++i) {
        const int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

void pack25519(uint8_t* o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; ++i) t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        const int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

void unpack25519(gf o, const uint8_t* n) {
    for (int i = 0; i < 16; ++i) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

void fA(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] + b[i]; }
void fZ(gf o, const gf a, const gf b) { for (int i = 0; i < 16; ++i) o[i] = a[i] - b[i]; }

void fM(gf o, const gf a, const gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; ++i) t[i] = 0;
    for (int i = 0; i < 16; ++i)
        for (int j = 0; j < 16; ++j) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; ++i) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; ++i) o[i] = t[i];
    car25519(o);
    car25519(o);
}

void fS(gf o, const gf a) { fM(o, a, a); }

void inv25519(gf o, const gf in) {
    gf c;
    for (int a = 0; a < 16; ++a) c[a] = in[a];
    for (int a = 253; a >= 0; --a) {
        fS(c, c);
        if (a != 2 && a != 4) fM(c, c, in);
    }
    for (int a = 0; a < 16; ++a) o[a] = c[a];
}

}  // namespace

namespace mesh_crypto {

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t off = 0;
    for (; off + 64 <= len; off += 64) shaBlock(h, data + off);

    // Final block(s): the tail, 0x80, zero padding, then the bit length
    // big-endian in the last eight bytes.
    uint8_t tail[128] = {};
    const size_t rem = len - off;
    memcpy(tail, data + off, rem);
    tail[rem] = 0x80;
    const size_t blocks = rem + 9 <= 64 ? 1 : 2;
    const uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i)
        tail[blocks * 64 - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t b = 0; b < blocks; ++b) shaBlock(h, tail + 64 * b);

    for (int i = 0; i < 8; ++i) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}

void clampPrivateKey(uint8_t k[32]) {
    k[0] &= 248;
    k[31] = (uint8_t)((k[31] & 127) | 64);
}

void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t z[32];
    int64_t x[80];
    gf a, b, c, d, e, f;
    memcpy(z, scalar, 32);
    clampPrivateKey(z);
    unpack25519(x, point);
    for (int i = 0; i < 16; ++i) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        const int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r);
        sel25519(c, d, r);
        fA(e, a, c);
        fZ(a, a, c);
        fA(c, b, d);
        fZ(b, b, d);
        fS(d, e);
        fS(f, a);
        fM(a, c, a);
        fM(c, b, e);
        fA(e, a, c);
        fZ(a, a, c);
        fS(b, a);
        fZ(c, d, f);
        fM(a, c, k121665);
        fA(a, a, d);
        fM(c, c, a);
        fM(a, d, f);
        fM(d, b, x);
        fS(b, e);
        sel25519(a, b, r);
        sel25519(c, d, r);
    }
    for (int i = 0; i < 16; ++i) {
        x[i + 16] = a[i];
        x[i + 32] = c[i];
    }
    inv25519(x + 32, x + 32);
    fM(x + 16, x + 16, x + 32);
    pack25519(out, x + 16);
    memset(z, 0, sizeof(z));
}

void x25519Public(uint8_t out[32], const uint8_t privateKey[32]) {
    static const uint8_t kBase[32] = {9};
    x25519(out, privateKey, kBase);
}

}  // namespace mesh_crypto
