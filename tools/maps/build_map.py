#!/usr/bin/env python3
"""OSM -> .ebm map builder for the e-paper bike computer.

Fetches ways from Overpass (cached), classifies them, simplifies geometry,
clips into a fixed lat/lon tile grid, and writes a compact binary the
firmware memory-maps from flash (or later, SD).

Format (little-endian):
  magic  'EBM2'
  f64    lat0, lon0        grid SW origin (deg)
  f64    tileDeg           tile size (deg, same in lat and lon)
  i32    nx, ny            grid dimensions
  u32[2] index[nx*ny]      (offset, length) of each tile, 0,0 = empty
  tiles:
    u16  polylineCount
    per polyline:
      u8   class            0 arterial, 1 secondary, 2 minor, 3 path
      u16  pointCount
      i16  x,y per point    meters east/north of the tile SW corner
  water section (appended after the tile data):
    magic  'WTR2'
    u16    polygonCount
    per polygon:
      u16  pointCount
      i16  x,y per point    meters east/north of the grid SW origin (lat0,lon0)

Usage:
  python3 build_map.py --bbox 37.703 -122.525 37.835 -122.355 \
      --name sf --out ../../data/sf.ebm
"""
import argparse
import gzip
import json
import math
import os
import struct
import sys
import urllib.request

OVERPASS = "https://overpass-api.de/api/interpreter"

QUERY = """
[out:json][timeout:300];
(
  way["highway"~"^(motorway|trunk|primary|secondary|tertiary|residential|unclassified|living_street|pedestrian|cycleway|footway|path|track|steps)"]({s},{w},{n},{e});
  way["natural"="water"]({s},{w},{n},{e});
);
out body;
>;
out skel qt;
"""

# Road tiers (device render classes).
ARTERIAL = {"motorway", "trunk", "primary"}
SECONDARY = {"secondary", "tertiary"}
MINOR = {"residential", "unclassified", "living_street", "pedestrian"}
PATH = {"cycleway", "footway", "path", "track", "steps"}


def classify(tags):
    # Rail/transit is dropped; natural=water is handled separately (WTR2), so
    # neither yields a road class here.
    hw = tags.get("highway", "")
    # Sidewalks and crossings duplicate every street — noise on a bike map.
    if hw == "footway" and tags.get("footway") in ("sidewalk", "crossing"):
        return None
    base = hw.split("_link")[0]
    if base in ARTERIAL:
        return 0
    if base in SECONDARY:
        return 1
    if base in MINOR:
        return 2
    if base in PATH:
        return 3
    return None


def fetch(bbox, cache):
    if os.path.exists(cache):
        print(f"using cached {cache}")
        with open(cache, "rb") as f:
            return json.load(f)
    s, w, n, e = bbox
    q = QUERY.format(s=s, w=w, n=n, e=e)
    print("querying Overpass (this can take a minute or two)...")
    req = urllib.request.Request(
        OVERPASS,
        data=("data=" + urllib.parse.quote(q)).encode(),
        headers={"Accept-Encoding": "gzip",
                 "User-Agent": "eink-bike-gps-map-builder"},
    )
    with urllib.request.urlopen(req, timeout=600) as resp:
        raw = resp.read()
        if resp.headers.get("Content-Encoding") == "gzip":
            raw = gzip.decompress(raw)
    with open(cache, "wb") as f:
        f.write(raw)
    print(f"cached {len(raw)/1e6:.1f} MB to {cache}")
    return json.loads(raw)


def rdp(points, eps):
    """Ramer-Douglas-Peucker on projected meter coords."""
    if len(points) < 3:
        return points
    stack = [(0, len(points) - 1)]
    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    while stack:
        a, b = stack.pop()
        ax, ay = points[a]
        bx, by = points[b]
        dx, dy = bx - ax, by - ay
        norm = math.hypot(dx, dy) or 1e-9
        worst, wi = 0.0, -1
        for i in range(a + 1, b):
            px, py = points[i]
            d = abs(dx * (ay - py) - dy * (ax - px)) / norm
            if d > worst:
                worst, wi = d, i
        if worst > eps:
            keep[wi] = True
            stack.append((a, wi))
            stack.append((wi, b))
    return [p for p, k in zip(points, keep) if k]


def decimate(points, eps):
    """Radial decimation for closed rings (water): keep the first point, then
    each point only if it's > eps metres from the last kept point. Unlike RDP,
    this survives closed rings (the implicit closing point drops at distance 0).
    """
    if not points:
        return points
    kept = [points[0]]
    lx, ly = points[0]
    for x, y in points[1:]:
        if math.hypot(x - lx, y - ly) > eps:
            kept.append((x, y))
            lx, ly = x, y
    return kept


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bbox", nargs=4, type=float, required=True,
                    metavar=("S", "W", "N", "E"))
    ap.add_argument("--name", default="map")
    ap.add_argument("--out", required=True)
    ap.add_argument("--tile-deg", type=float, default=0.02)
    ap.add_argument("--simplify-m", type=float, default=3.0)
    args = ap.parse_args()

    s, w, n, e = args.bbox
    data = fetch((s, w, n, e), f"{args.name}_overpass.json")

    nodes = {}
    ways = []
    water_ways = []  # node-id lists for natural=water polygons
    for el in data["elements"]:
        if el["type"] == "node":
            nodes[el["id"]] = (el["lat"], el["lon"])
        elif el["type"] == "way":
            tags = el.get("tags", {})
            cls = classify(tags)
            if cls is not None:
                ways.append((cls, el["nodes"]))
            elif tags.get("natural") == "water":
                water_ways.append(el["nodes"])
    print(f"{len(ways)} ways, {len(water_ways)} water, {len(nodes)} nodes")

    mid_lat = (s + n) / 2
    kx = 111320.0 * math.cos(math.radians(mid_lat))  # m per deg lon
    ky = 110540.0                                    # m per deg lat

    td = args.tile_deg
    lat0 = math.floor(s / td) * td
    lon0 = math.floor(w / td) * td
    nx = int(math.ceil((e - lon0) / td))
    ny = int(math.ceil((n - lat0) / td))
    print(f"grid {nx} x {ny} tiles of {td} deg")

    tiles = {}  # (tx,ty) -> list of (cls, [(mx,my) tile-local meters])
    dropped = 0

    for cls, nids in ways:
        pts = [nodes[i] for i in nids if i in nodes]
        if len(pts) < 2:
            continue
        # project to meters relative to grid origin, simplify
        m = [((lon - lon0) * kx, (lat - lat0) * ky) for lat, lon in pts]
        m = rdp(m, args.simplify_m)

        # split into per-tile runs; adjacent runs share the boundary point
        # so lines continue across tile edges
        def tile_of(p):
            return (int(p[0] // (td * kx)), int(p[1] // (td * ky)))

        run = [m[0]]
        cur = tile_of(m[0])
        for p in m[1:]:
            t = tile_of(p)
            run.append(p)
            if t != cur:
                emit(tiles, cur, cls, run, td, kx, ky, nx, ny)
                run = [run[-2], p]
                cur = t
        emit(tiles, cur, cls, run, td, kx, ky, nx, ny)

    # serialize
    header_size = 4 + 8 * 3 + 4 * 2
    index_size = nx * ny * 8
    blobs = {}
    for key, polys in tiles.items():
        out = bytearray()
        out += struct.pack("<H", len(polys))
        for cls, pts in polys:
            out += struct.pack("<BH", cls, len(pts))
            for x, y in pts:
                out += struct.pack("<hh", x, y)
        blobs[key] = bytes(out)

    off = header_size + index_size
    index = bytearray()
    order = []
    for ty in range(ny):
        for tx in range(nx):
            b = blobs.get((tx, ty))
            if b:
                index += struct.pack("<II", off, len(b))
                order.append(b)
                off += len(b)
            else:
                index += struct.pack("<II", 0, 0)

    # Water polygons (natural=water) -> WTR2 section. Points are meters E/N of
    # the grid SW origin (lat0,lon0) — the same snapped origin the road grid
    # uses. A polygon is kept if any of its points fall in [s,w,n,e]; the whole
    # simplified ring is stored (no clipping).
    water_polys = []
    for nids in water_ways:
        pts = [nodes[i] for i in nids if i in nodes]
        if not any(s <= lat <= n and w <= lon <= e for lat, lon in pts):
            continue
        m = [((lon - lon0) * kx, (lat - lat0) * ky) for lat, lon in pts]
        # Radial decimation (NOT RDP): keep a point only if it's > simplify_m
        # from the last kept point. Survives closed rings; the implicit closing
        # point (equal to the first) naturally drops at distance 0. The device
        # closes the ring, so we never append a closing point.
        m = decimate(m, args.simplify_m)
        if len(m) < 3:
            continue
        poly = []
        for x, y in m:
            ix = max(-32000, min(32000, int(round(x))))
            iy = max(-32000, min(32000, int(round(y))))
            poly.append((ix, iy))
        water_polys.append(poly)

    with open(args.out, "wb") as f:
        f.write(b"EBM2")
        f.write(struct.pack("<ddd", lat0, lon0, td))
        f.write(struct.pack("<ii", nx, ny))
        f.write(index)
        for b in order:
            f.write(b)
        # WTR2 water section
        f.write(b"WTR2")
        f.write(struct.pack("<H", len(water_polys)))
        for poly in water_polys:
            f.write(struct.pack("<H", len(poly)))
            for x, y in poly:
                f.write(struct.pack("<hh", x, y))

    total_pts = sum(len(p) for polys in tiles.values() for _, p in polys)
    print(f"wrote {args.out}: {off/1e6:.2f} MB, "
          f"{sum(len(v) for v in tiles.values())} polylines, {total_pts} points")
    if dropped:
        print(f"note: {dropped} out-of-grid segments dropped")


def emit(tiles, key, cls, run, td, kx, ky, nx, ny):
    if len(run) < 2:
        return
    tx, ty = key
    if tx < 0 or tx >= nx or ty < 0 or ty >= ny:
        return
    ox, oy = tx * td * kx, ty * td * ky
    pts = []
    for x, y in run:
        lx, ly = x - ox, y - oy
        # clamp to int16; points just past the boundary stay representable
        lx = max(-32000, min(32000, int(round(lx))))
        ly = max(-32000, min(32000, int(round(ly))))
        pts.append((lx, ly))
    # drop consecutive duplicates from rounding
    dedup = [pts[0]]
    for p in pts[1:]:
        if p != dedup[-1]:
            dedup.append(p)
    if len(dedup) >= 2:
        tiles.setdefault(key, []).append((cls, dedup))


if __name__ == "__main__":
    main()
