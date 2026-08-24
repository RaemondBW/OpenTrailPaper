#!/usr/bin/env python3
"""Decode emulator frame streams (epd_compat_emu.cpp's UART1 protocol) to PNGs.

Usage: frame2png.py <stream.bin> <out.png>   — renders the LAST complete frame.
The buffer is epdiy-native: 960x540 landscape, 4bpp, two pixels per byte with
the LOW nibble at even x; the UI draws portrait, so the PNG is rotated to the
540x960 the panel shows.
"""
import sys

W, H = 960, 540
FB = W * H // 2


def frames(data: bytes):
    i = 0
    while True:
        i = data.find(b"\xf5F", i)
        if i < 0 or i + 8 > len(data):
            return
        seq = data[i + 2] | (data[i + 3] << 8)
        rle_len = int.from_bytes(data[i + 4 : i + 8], "little")
        end = i + 8 + rle_len
        if end + 1 > len(data):
            return
        if data[end] != 0xF6:   # desync — resume scanning after the marker
            i += 2
            continue
        yield seq, data[i + 8 : end]
        i = end + 1


def expand(rle: bytes) -> bytearray:
    out = bytearray()
    for i in range(0, len(rle) - 1, 2):
        out += bytes([rle[i + 1]]) * rle[i]
    return out[:FB]


def main():
    data = open(sys.argv[1], "rb").read()
    last = None
    for seq, rle in frames(data):
        last = (seq, rle)
    if not last:
        sys.exit("no complete frame in stream")
    seq, rle = last
    fb = expand(rle)
    if len(fb) < FB:
        sys.exit(f"frame {seq} truncated: {len(fb)}/{FB}")

    # 4bpp -> 8bpp greyscale, native landscape.
    px = bytearray(W * H)
    for i, b in enumerate(fb):
        px[i * 2] = (b & 0x0F) * 17
        px[i * 2 + 1] = (b >> 4) * 17

    # Rotate to the portrait the UI laid out (90° ccw: native (x,y) lands at
    # portrait row W-1-x — verified against the first live frame, whose power
    # dialog came out upside down the other way).
    rot = bytearray(W * H)
    for y in range(H):
        for x in range(W):
            rot[(W - 1 - x) * H + y] = px[y * W + x]

    try:
        from PIL import Image
        Image.frombytes("L", (H, W), bytes(rot)).save(sys.argv[2])
    except ImportError:
        # Minimal PNG writer fallback (no deps): pack as PGM instead.
        out = sys.argv[2].replace(".png", ".pgm")
        with open(out, "wb") as f:
            f.write(f"P5 {H} {W} 255\n".encode())
            f.write(bytes(rot))
        print(f"PIL missing — wrote {out}")
        return
    print(f"frame {seq} -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
