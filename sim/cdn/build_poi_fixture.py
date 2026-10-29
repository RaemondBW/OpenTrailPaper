#!/usr/bin/env python3
"""Build the cycling-POI sets of the app map fixture (sim/fixtures/appmaps/pois).

    sim/cdn/build_poi_fixture.py [--fixture sim/fixtures/appmaps]

The fixture's own .poi files are the CDN's, and the real Woodstock area has a
single POI in it. These sets give the POI tests something to send: a handful
of drinking water, toilets, repair stands and bike shops (made up, with the
tags OSM uses for them), written per set as

    pois/<set>/overpass.json   what Overpass answers the apps' POI query
                               (`out tags center`) for the whole area
    pois/<set>/<id>.poi        each fixture cell's .poi, built from it by the
                               reference writer, docs/mapgen.js buildPoi, with
                               each POI in the one H3 cell containing it (the
                               apps' own H3 C, via h3cover.c)

A cell with no POIs still gets its (empty, 40-byte) file: it is what the apps
synthesise for a CDN index entry with poiSize 0, and what the device must hold.
The stand-in CDN serves a set when a test asks (tile_cdn.py /_ctl/pois): the
non-empty files as the CDN's .poi files, with their sizes and hashes in the
group index, and overpass.json as Overpass's answer to a POI query.

Sets:
    woodstock   the POIs below
    changed     the same, with one cell's POIs changed (a new repair stand,
                and the old one's pump gone) — an upstream edit
    none        no POIs anywhere in the area

Needs node (docs/mapgen.js) and cc (h3cover.c).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
from fetch_fixture import build_h3cover  # noqa: E402

# Around the fixture's centre (41.9697, -72.0397), all inside its box
# (+-0.04 lat, +-0.05 lon) so every one lands in a fixture cell.
WOODSTOCK = [
    # The one a test centres on: 150 m due north of the area's centre.
    {"type": "node", "id": 9100000001, "lat": 41.97105, "lon": -72.0397,
     "tags": {"amenity": "drinking_water", "name": "Common water fountain"}},
    {"type": "node", "id": 9100000002, "lat": 41.9672, "lon": -72.0445,
     "tags": {"amenity": "toilets", "drinking_water": "yes", "access": "yes"}},
    {"type": "node", "id": 9100000003, "lat": 41.9745, "lon": -72.0352,
     "tags": {"amenity": "bicycle_repair_station", "service:bicycle:pump": "yes",
              "service:bicycle:tools": "yes", "service:bicycle:stand": "yes"}},
    {"type": "node", "id": 9100000004, "lat": 41.9620, "lon": -72.0290,
     "tags": {"shop": "bicycle", "name": "Quiet Corner Cycles", "service:bicycle:repair": "yes",
              "service:bicycle:retail": "yes", "service:bicycle:rental": "yes"}},
    # A toilet block mapped as a building: Overpass gives its centre.
    {"type": "way", "id": 9200000005, "center": {"lat": 41.9580, "lon": -72.0500},
     "nodes": [9300000001, 9300000002, 9300000003, 9300000004, 9300000001],
     "tags": {"amenity": "toilets", "building": "yes", "seasonal": "summer"}},
    {"type": "node", "id": 9100000006, "lat": 42.0000, "lon": -72.0000,
     "tags": {"man_made": "water_tap", "drinking_water": "yes"}},
    {"type": "node", "id": 9100000007, "lat": 41.9450, "lon": -72.0000,
     "tags": {"amenity": "fountain", "drinking_water": "yes", "fee": "no"}},
    {"type": "node", "id": 9100000008, "lat": 41.9950, "lon": -72.0750,
     "tags": {"amenity": "drinking_water", "access": "customers"}},
    {"type": "node", "id": 9100000009, "lat": 41.9350, "lon": -72.0800,
     "tags": {"shop": "bicycle", "service:bicycle:second_hand": "yes", "service:bicycle:ebike": "yes",
              "service:bicycle:pump": "yes"}},
    # Not kept by any builder: private, and a tap that is not drinking water.
    {"type": "node", "id": 9100000010, "lat": 41.9690, "lon": -72.0420,
     "tags": {"amenity": "toilets", "access": "private"}},
    {"type": "node", "id": 9100000011, "lat": 41.9705, "lon": -72.0360,
     "tags": {"man_made": "water_tap", "drinking_water": "no"}},
]


def changed():
    els = json.loads(json.dumps(WOODSTOCK))
    for el in els:
        if el["id"] == 9100000003:
            del el["tags"]["service:bicycle:pump"]
    els.append({"type": "node", "id": 9100000012, "lat": 41.9650, "lon": -72.0330,
                "tags": {"amenity": "bicycle_repair_station", "service:bicycle:pump": "yes"}})
    return els


SETS = {"woodstock": WOODSTOCK, "changed": changed(), "none": []}


def overpass(elements):
    return {"version": 0.6, "generator": "Overpass API (sim fixture, build_poi_fixture.py)",
            "osm3s": {"timestamp_osm_base": "2026-10-08T20:21:06Z",
                      "copyright": "Made-up POIs for the OpenTrailPaper sim tests."},
            "elements": elements}


JS = r"""
import fs from "node:fs";
const { mapgen, json, cells, at } = JSON.parse(fs.readFileSync(0, "utf8"));
const { buildPoi } = await import(mapgen);
const cellOf = new Map(at.map(([la, lo, id]) => [`${la},${lo}`, id]));
const out = {};
for (const c of cells) {
  const b = buildPoi(json, { s: c.s, w: c.w, n: c.n, e: c.e, cell: c.id,
                             contains: (la, lo) => cellOf.get(`${la},${lo}`) === c.id });
  out[c.id] = Buffer.from(b).toString("hex");
}
process.stdout.write(JSON.stringify(out));
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fixture", default=os.path.join(ROOT, "sim", "fixtures", "appmaps"))
    a = ap.parse_args()
    area = json.load(open(os.path.join(a.fixture, "area.json")))
    cells = area["cells"]
    ids = {c["id"] for c in cells}

    tmp = tempfile.mkdtemp()
    try:
        cover = build_h3cover(tmp)
        out_dir = os.path.join(a.fixture, "pois")
        for name, elements in SETS.items():
            points = [(el["lat"], el["lon"]) if "lat" in el else (el["center"]["lat"], el["center"]["lon"])
                      for el in elements]
            at = []
            if points:
                args = [str(v) for p in points for v in p]
                got = subprocess.run([cover, "at", *args], check=True, capture_output=True, text=True).stdout.split()
                at = [[la, lo, cid] for (la, lo), cid in zip(points, got)]
                stray = [p for p in at if p[2] not in ids]
                if stray:
                    sys.exit(f"{name}: POIs outside the fixture's cells: {stray}")
            js = os.path.join(tmp, "build.mjs")
            open(js, "w").write(JS)
            spec = {"mapgen": "file://" + os.path.join(ROOT, "docs", "mapgen.js"), "json": overpass(elements),
                    "cells": cells, "at": at}
            r = subprocess.run(["node", js], input=json.dumps(spec), check=True, capture_output=True, text=True)
            files = {k: bytes.fromhex(v) for k, v in json.loads(r.stdout).items()}
            d = os.path.join(out_dir, name)
            shutil.rmtree(d, ignore_errors=True)
            os.makedirs(d)
            with open(os.path.join(d, "overpass.json"), "w") as f:
                json.dump(overpass(elements), f, indent=1)
                f.write("\n")
            for cid, body in sorted(files.items()):
                open(os.path.join(d, f"{cid}.poi"), "wb").write(body)
            counts = {cid: int.from_bytes(b[6:8], "little") for cid, b in files.items()}
            print(f"{name}: " + ", ".join(f"{cid} {n}" for cid, n in sorted(counts.items())))
    finally:
        shutil.rmtree(tmp)


if __name__ == "__main__":
    main()
