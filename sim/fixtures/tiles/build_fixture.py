#!/usr/bin/env python3
"""Rebuild the real-tile fixture (sim/fixtures/tiles/) from the prebuilt-tile CDN.

    python3 sim/fixtures/tiles/build_fixture.py            # Hood River, OR
    python3 sim/fixtures/tiles/build_fixture.py --radius-km 9 --lat .. --lon ..

Fetches what the apps fetch (companion-ios/Sources/PrebuiltTiles.swift): each
group's <g>/index.json, then <g>/<h3>.ebm?v=<hash> and <g>/<h3>.poi?v=<hash>
for every res-6 cell a disc around the centre touches. The cells are found
with the apps' own H3 C code (tools/tiles/h3tool.c, built into a temp dir), so
the ids are the ones the firmware's h3_cell_at() computes. Every body is
checked as the apps check it (size = index size, magic) and against the
index hash (sha256[:16], tools/tiles/build_region.mjs).

Only read-only GETs, only when (re)building the fixture: the tests read the
committed files and run offline. The tiles are OpenStreetMap data (ODbL),
see README.md.
"""
import argparse
import hashlib
import json
import math
import os
import subprocess
import sys
import tempfile
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
BASE = "https://tiles.opentrailpaper.com/v1/"
# Cloudflare answers a bare client default (curl, urllib) with 403; the apps
# send their own name too (MapBuilder.userAgent).
UA = "OpenTrailPaper/sim-fixture (sim/fixtures/tiles/build_fixture.py)"

# Downtown Hood River, Oregon: a small town on the Columbia River (water fill
# on the WA/OR line), a grid of residential streets, I-84 and SR-35, and the
# Post Canyon / Indian Creek trails to the south-west.
DEFAULT = dict(name="hood-river", lat=45.7054, lon=-121.5215, radius_km=9.0)


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read()


def h3tool():
    out = os.path.join(tempfile.mkdtemp(prefix="h3tool-"), "h3tool")
    subprocess.run(["sh", os.path.join(ROOT, "tools", "tiles", "build_h3tool.sh"), out],
                   check=True, stdout=subprocess.DEVNULL)
    return out


def run(tool, mode, lines):
    r = subprocess.run([tool, mode], input="\n".join(lines) + "\n", capture_output=True, text=True,
                       check=True)
    return r.stdout.splitlines()


def offset(lat, lon, north_m, east_m):
    return (lat + north_m / 111320.0, lon + east_m / (111320.0 * math.cos(math.radians(lat))))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", default=DEFAULT["name"])
    ap.add_argument("--lat", type=float, default=DEFAULT["lat"])
    ap.add_argument("--lon", type=float, default=DEFAULT["lon"])
    ap.add_argument("--radius-km", type=float, default=DEFAULT["radius_km"])
    ap.add_argument("--out", default=HERE)
    a = ap.parse_args()

    tool = h3tool()
    # Every cell a dense polar sample of the disc lands in (res-6 cells are
    # ~3.7 km across an edge, so 300 m steps miss none).
    pts = [(a.lat, a.lon)]
    for r in range(300, int(a.radius_km * 1000) + 1, 300):
        for deg in range(0, 360, 5):
            pts.append(offset(a.lat, a.lon, r * math.cos(math.radians(deg)), r * math.sin(math.radians(deg))))
    cells = sorted(set(run(tool, "cell", ["%.8f %.8f" % p for p in pts])) - {"0"})
    centre = run(tool, "cell", ["%.8f %.8f" % (a.lat, a.lon)])[0]
    bboxes = {}
    for line in run(tool, "bbox", cells):
        h, s, w, n, e = line.split()
        bboxes[h] = [float(s), float(w), float(n), float(e)]

    indexes = {}
    for g in sorted({c[:6] for c in cells}):
        indexes[g] = json.loads(get(BASE + g + "/index.json"))
        assert indexes[g].get("v") == 1, g
    meta = json.loads(get(BASE + "meta.json"))

    manifest_cells = {}
    total = 0
    for h in cells:
        e = indexes[h[:6]]["cells"].get(h)
        if not e or e[0] == 0:
            print("skip %s: %s" % (h, "not built" if not e else "nothing to draw"))
            continue
        ebm_size, ebm_hash, poi_size, poi_hash = e[:4]
        ebm = get("%s%s/%s.ebm?v=%s" % (BASE, h[:6], h, ebm_hash))
        assert len(ebm) == ebm_size and ebm[:4] == b"EBM2", h
        assert hashlib.sha256(ebm).hexdigest()[:16] == ebm_hash, h
        with open(os.path.join(a.out, h + ".ebm"), "wb") as f:
            f.write(ebm)
        total += len(ebm)
        rec = {"ebm_size": ebm_size, "ebm_hash": ebm_hash, "ebm_version": "cdn:" + ebm_hash,
               "bbox": bboxes[h]}
        if poi_size:
            poi = get("%s%s/%s.poi?v=%s" % (BASE, h[:6], h, poi_hash))
            assert len(poi) == poi_size and poi[:4] == b"EPOI", h
            assert hashlib.sha256(poi).hexdigest()[:16] == poi_hash, h
            with open(os.path.join(a.out, h + ".poi"), "wb") as f:
                f.write(poi)
            total += len(poi)
            rec.update(poi_size=poi_size, poi_hash=poi_hash, poi_version="cdn:" + poi_hash)
        manifest_cells[h] = rec
        print("%s %6d B ebm %s" % (h, ebm_size, "+ %d B poi" % poi_size if poi_size else ""))

    s = min(c["bbox"][0] for c in manifest_cells.values())
    w = min(c["bbox"][1] for c in manifest_cells.values())
    n = max(c["bbox"][2] for c in manifest_cells.values())
    e = max(c["bbox"][3] for c in manifest_cells.values())
    # A ride for the panning test: west to east through the centre, 10 km,
    # with the cells it passes through in order; and a point well outside.
    track = [offset(a.lat, a.lon, 0, d) for d in range(-5000, 5001, 50)]
    crossed = []
    for c in run(tool, "cell", ["%.8f %.8f" % p for p in track]):
        if not crossed or crossed[-1] != c:
            crossed.append(c)
    # What a 2 m/px map page sees along it (+-600 m E/W, +-1 km N/S) must be in the fixture.
    seen = set(run(tool, "cell", ["%.8f %.8f" % offset(la, lo, dn, de) for la, lo in track[::10]
                                  for dn in (-1000, 0, 1000) for de in (-600, 0, 600)]))
    assert seen <= set(manifest_cells), sorted(seen - set(manifest_cells))
    outside = offset(a.lat, a.lon, 0, -45000)
    assert run(tool, "cell", ["%.8f %.8f" % outside])[0] not in manifest_cells

    regions = sorted({r for g in indexes.values() for r in g.get("regions", [])})
    osm = {r: meta.get("fragments", {}).get(r, {}).get("osm") for r in regions}
    manifest = {
        "name": a.name,
        "source": BASE,
        "fetched": time.strftime("%Y-%m-%d", time.gmtime()),
        "osm_snapshot": osm,
        "license": "ODbL 1.0, (c) OpenStreetMap contributors (see README.md)",
        "centre": {"lat": a.lat, "lon": a.lon, "h3": centre},
        "radius_km": a.radius_km,
        "bbox": {"s": s, "w": w, "n": n, "e": e},
        "cells": manifest_cells,
        "track": {"from": [round(x, 7) for x in track[0]], "to": [round(x, 7) for x in track[-1]],
                  "cells": crossed},
        "outside": {"lat": round(outside[0], 7), "lon": round(outside[1], 7)},
    }
    with open(os.path.join(a.out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
        f.write("\n")
    print("%d cells, %d KB" % (len(manifest_cells), total // 1024))


if __name__ == "__main__":
    sys.exit(main())
