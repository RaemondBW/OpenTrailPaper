#!/usr/bin/env python3
"""Fetch a small real-tile fixture from the public tile CDN, once.

    sim/cdn/fetch_fixture.py                       # the committed fixture
    sim/cdn/fetch_fixture.py --center 41.97 -72.04 --out /tmp/fx

Writes, under --out (default sim/fixtures/appmaps):

    v1/<g>/index.json      the group indexes, cut down to the fixture's cells
    v1/<g>/<id>.ebm|.poi   the tiles and POI files, byte for byte
    v1/meta.json           meta.json, cut down to the fragments those cells use
    area.json              the area: centre, box, and each cell's id + bbox

The cells are the ones the apps' own H3 cover (h3cover.c, built here from
companion-ios/Sources/H3) gives for a box of +-DLAT/DLON around the centre:
a selection anywhere inside it gets only fixture hexes, so the apps never
fall back to Overpass for it. Each file is checked against its index entry
(size, and sha256[:16] = the hash, tools/tiles/build_region.mjs).

Read-only GETs, with the app's User-Agent (the CDN's bot filter turns away
curl's). Run it again only to refresh the fixture; the tests never touch the
network. The data is OpenStreetMap's (ODbL): see the fixture's README.md.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CDN = "https://tiles.opentrailpaper.com/v1/"
UA = "OpenTrailPaper/1.3 (sim fixture)"
# Woodstock, CT: rural roads with a few POIs, small tiles (7 hexes ~ 160 KB).
CENTER = (41.9697, -72.0397)
DLAT, DLON = 0.04, 0.05
MAX_BYTES = 2 * 1024 * 1024


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read()


def build_h3cover(tmp):
    h3 = os.path.join(ROOT, "companion-ios", "Sources", "H3")
    out = os.path.join(tmp, "h3cover")
    srcs = [os.path.join(os.path.dirname(__file__), "h3cover.c"), os.path.join(h3, "h3shim.c")]
    srcs += sorted(os.path.join(h3, "lib", f) for f in os.listdir(os.path.join(h3, "lib")) if f.endswith(".c"))
    subprocess.run([os.environ.get("CC", "cc"), "-O2", "-std=c11", "-D_DEFAULT_SOURCE",
                    "-I", h3, "-I", os.path.join(h3, "include"), *srcs, "-lm", "-o", out], check=True)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--center", nargs=2, type=float, default=CENTER, metavar=("LAT", "LON"))
    ap.add_argument("--dlat", type=float, default=DLAT)
    ap.add_argument("--dlon", type=float, default=DLON)
    ap.add_argument("--out", default=os.path.join(ROOT, "sim", "fixtures", "appmaps"))
    a = ap.parse_args()
    lat, lon = a.center
    box = [lat - a.dlat, lon - a.dlon, lat + a.dlat, lon + a.dlon]

    tmp = tempfile.mkdtemp()
    try:
        cover = build_h3cover(tmp)
        lines = subprocess.run([cover, *map(str, box)], check=True, capture_output=True, text=True).stdout
    finally:
        shutil.rmtree(tmp)
    cells = []
    for line in lines.splitlines():
        p = line.split()
        cells.append({"id": p[0], "s": float(p[1]), "w": float(p[2]), "n": float(p[3]), "e": float(p[4])})
    cells.sort(key=lambda c: c["id"])
    print(f"{len(cells)} cells cover {box}")

    staging = a.out + ".new"
    shutil.rmtree(staging, ignore_errors=True)
    total = 0
    regions = set()
    for g in sorted({c["id"][:6] for c in cells}):
        index = json.loads(get(f"{CDN}{g}/index.json"))
        mine = [c["id"] for c in cells if c["id"][:6] == g]
        missing = [i for i in mine if i not in index["cells"]]
        if missing:
            sys.exit(f"not pre-built on the CDN (pick another centre): {missing}")
        os.makedirs(os.path.join(staging, "v1", g))
        regions.update(index.get("regions", []))
        cut = {"v": index["v"], "cells": {i: index["cells"][i] for i in mine}, "regions": index.get("regions", [])}
        data = json.dumps(cut, separators=(",", ":")).encode()
        open(os.path.join(staging, "v1", g, "index.json"), "wb").write(data)
        total += len(data)
        for i in mine:
            ebm_size, ebm_hash, poi_size, poi_hash = index["cells"][i]
            for ext, size, h in (("ebm", ebm_size, ebm_hash), ("poi", poi_size, poi_hash)):
                if size == 0:
                    continue
                body = get(f"{CDN}{g}/{i}.{ext}?v={h}")
                if len(body) != size:
                    sys.exit(f"{i}.{ext}: {len(body)} bytes, index says {size}")
                if hashlib.sha256(body).hexdigest()[:16] != h:
                    print(f"warning: {i}.{ext} sha256[:16] is not its index hash {h}")
                open(os.path.join(staging, "v1", g, f"{i}.{ext}"), "wb").write(body)
                total += len(body)
            print(f"  {i}  ebm {ebm_size:>7}  poi {poi_size:>5}")

    meta = json.loads(get(f"{CDN}meta.json"))
    meta["fragments"] = {k: v for k, v in meta.get("fragments", {}).items() if k in regions}
    data = json.dumps(meta, indent=1).encode()
    open(os.path.join(staging, "v1", "meta.json"), "wb").write(data)
    total += len(data)

    area = {"center": [lat, lon], "box": box, "cells": cells,
            "source": CDN, "osm": {k: v.get("osm") for k, v in meta["fragments"].items()}}
    open(os.path.join(staging, "area.json"), "w").write(json.dumps(area, indent=1) + "\n")
    if total > MAX_BYTES:
        sys.exit(f"{total} bytes is over the fixture budget ({MAX_BYTES}); use a smaller box")

    readme = os.path.join(a.out, "README.md")
    keep = open(readme).read() if os.path.exists(readme) else None
    shutil.rmtree(a.out, ignore_errors=True)
    os.rename(staging, a.out)
    if keep is not None:
        open(readme, "w").write(keep)
    print(f"wrote {a.out}: {total} bytes")


if __name__ == "__main__":
    main()
