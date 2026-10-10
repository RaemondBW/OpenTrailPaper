#pragma once

// The two public-key primitives Meshtastic's PKI direct messages are built on:
// X25519 (RFC 7748) for the key agreement and SHA-256 to turn the shared point
// into an AES-256 key. Meshtastic 2.5+ gives every node a Curve25519 key pair,
// advertises the public half in User.public_key, and encrypts a direct message
// with AES-256-CCM under SHA256(X25519(our private, their public)) — see
// mesh_proto.h for the packet format.
//
// Portable C++ with no Arduino or ESP-IDF dependency, for the same reason as
// mesh_proto.cpp: tools/mesh_test pins both against RFC 7748 / FIPS 180-2
// vectors on the host, where a wrong carry in the field arithmetic shows up as a
// failed test rather than as DMs that silently never decrypt on the trail.
//
// The X25519 here is the TweetNaCl formulation (public domain): constant-time,
// small, and slow by desktop standards but a few tens of milliseconds on the
// ESP32-S3, which is nothing next to a LongFast packet's second of airtime. It
// needs roughly 1.8 KB of stack — the mesh task is sized for it.

#include <cstddef>
#include <cstdint>

namespace mesh_crypto {

void sha256(const uint8_t* data, size_t len, uint8_t out[32]);

// out = scalar * point. The scalar is clamped internally (RFC 7748 §5), so a
// stored private key works whether or not it was clamped when generated.
void x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);

// out = scalar * basepoint — the public key for a private key.
void x25519Public(uint8_t out[32], const uint8_t privateKey[32]);

// Applies RFC 7748 clamping in place. Meshtastic stores its private key clamped
// (Curve25519::dh1 does it), so this firmware does too.
void clampPrivateKey(uint8_t k[32]);

}  // namespace mesh_crypto
