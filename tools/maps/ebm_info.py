#!/usr/bin/env python3
"""Summarise .ebm map tiles and their .poi files: where the bytes go.

  python3 tools/maps/ebm_info.py <file.ebm | file.poi | tile dir> [...]

Walks a file (or every .ebm / .poi under a directory, e.g. an unzipped
maps/tiles/ tree) the same way src/map_tiles.cpp does and prints totals:
road records and points, the cycling way-flag trailers (and how many ways
carry each flag), ELV1 / WTR2 / PRK2, any extension sections after PRK2, and
the .poi files (POI counts by type). Used for the size numbers in
investigations/osm-pois-bike-routes.md.
"""
import os
import struct
import sys
from collections import Counter

POI_NAMES = {1: "water", 2: "toilets", 3: "repair", 4: "bike_shop"}


def parse(b, tot):
    if len(b) < 36 or b[:4] != b"EBM2":
        tot["bad"] += 1
        return
    nx, ny = struct.unpack_from("<ii", b, 28)
    tot["files"] += 1
    tot["bytes"] += len(b)
    tot["header+index"] += 36 + nx * ny * 8
    max_end = 36 + nx * ny * 8
    for k in range(nx * ny):
        off, ln = struct.unpack_from("<II", b, 36 + k * 8)
        if not off:
            continue
        max_end = max(max_end, off + ln)
        end = off + ln
        (count,) = struct.unpack_from("<H", b, off)
        p = off + 2
        for _ in range(count):
            n = struct.unpack_from("<H", b, p + 1)[0]
            p += 3 + n * 4
            tot["road records"] += 1
            tot["road points"] += n
        tot["road bytes"] += p - off
        if end - p == count and count:
            tot["trailer bytes"] += count
            tot["sub-tiles with trailer"] += 1
            for f in b[p:end]:
                lvl = f & 3
                if lvl:
                    tot["flag route lvl%d" % lvl] += 1
                if f & 4:
                    tot["flag cycleway"] += 1
                if f & 8:
                    tot["flag bike lane"] += 1
        elif end != p:
            tot["sub-tiles with unknown trailer"] += 1
    q = max_end
    if q + 44 <= len(b) and b[q:q + 4] == b"ELV1":
        gw, gh = struct.unpack_from("<ii", b, q + 4)
        tot["ELV1 bytes"] += 44 + gw * gh * 2
        q += 44 + gw * gh * 2
    for magic in (b"WTR2", b"PRK2"):
        if q + 6 > len(b) or b[q:q + 4] != magic:
            break
        start = q
        (pc,) = struct.unpack_from("<H", b, q + 4)
        q += 6
        for _ in range(pc):
            q += 2 + struct.unpack_from("<H", b, q)[0] * 4
        tot[magic.decode() + " bytes"] += q - start
    while q + 8 <= len(b):
        magic = b[q:q + 4]
        (ln,) = struct.unpack_from("<I", b, q + 4)
        body = q + 8
        if body + ln > len(b):
            tot["truncated section"] += 1
            break
        name = magic.decode("ascii", "replace")
        tot["section " + name + " bytes"] += 8 + ln
        q = body + ln
    if q != len(b):
        tot["trailing bytes"] += len(b) - q


def parse_poi(b, tot):
    # 'EPOI' u8 version u8 recordSize u16 count u64 cell f64 lat0 lon0 f32 kx ky
    if len(b) < 40 or b[:4] != b"EPOI" or b[4] != 1:
        tot["bad .poi"] += 1
        return
    rec, count = b[5], struct.unpack_from("<H", b, 6)[0]
    tot[".poi files"] += 1
    tot[".poi bytes"] += len(b)
    for i in range(count):
        t, f = b[40 + i * rec], b[41 + i * rec]
        tot["poi " + POI_NAMES.get(t, "type%d" % t)] += 1
        if f & 0x80:
            tot["poi restricted"] += 1


def main():
    tot = Counter()
    for arg in sys.argv[1:]:
        paths = [arg]
        if os.path.isdir(arg):
            paths = [os.path.join(d, f) for d, _, fs in os.walk(arg)
                     for f in fs if f.endswith((".ebm", ".poi"))]
        for p in sorted(paths):
            with open(p, "rb") as fh:
                (parse_poi if p.endswith(".poi") else parse)(fh.read(), tot)
    width = max(len(k) for k in tot) if tot else 0
    for k in sorted(tot):
        print("%-*s %10d" % (width, k, tot[k]))


if __name__ == "__main__":
    main()
