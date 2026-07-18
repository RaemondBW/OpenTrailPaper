"""Rigorous correctness test for region_sea_polygons: sample known SF points
(ocean / bay / land) and assert each falls on the right side of the sea fill."""
import json
import sys
import build_map as bm

S, W, N, E = 37.70, -122.52, 37.83, -122.35
# build_map ran from the repo root with default --name "map", so its Overpass
# cache is ../../map_overpass.json relative to this file.
import os
_here = os.path.dirname(os.path.abspath(__file__))
data = json.load(open(os.path.join(_here, "..", "..", "map_overpass.json")))

nodes = {}
coast_ways = []
for el in data["elements"]:
    if el["type"] == "node":
        nodes[el["id"]] = (el["lat"], el["lon"])
    elif el["type"] == "way" and el.get("tags", {}).get("natural") == "coastline":
        coast_ways.append(el["nodes"])
chains = bm.assemble_coastline(coast_ways, nodes)
rings = bm.region_sea_polygons(chains, S, W, N, E)
print(f"{len(coast_ways)} coast ways -> {len(chains)} chains -> {len(rings)} sea rings")


def in_sea(lat, lon):
    return any(bm._point_in_ring(r, lat, lon) for r in rings)


# label, lat, lon, expected_sea
cases = [
    ("Ocean Beach surf (Pacific)",   37.755, -122.512, True),
    ("Pacific, offshore SW",         37.730, -122.515, True),
    ("Bay, mid Bay Bridge",          37.800, -122.372, True),
    ("Bay, off Embarcadero",         37.805, -122.385, True),
    ("Golden Gate (north strait)",   37.820, -122.470, True),
    ("Downtown / FiDi (land)",       37.792, -122.402, False),
    ("Mission District (land)",      37.760, -122.415, False),
    ("Golden Gate Park (land)",      37.769, -122.483, False),
    ("Twin Peaks (land)",            37.752, -122.447, False),
    ("Sunset District (land)",       37.745, -122.490, False),
    ("Bernal Heights (land)",        37.740, -122.415, False),
]
bad = 0
for label, lat, lon, exp in cases:
    got = in_sea(lat, lon)
    ok = got == exp
    if not ok:
        bad += 1
    print(f"  [{'OK ' if ok else 'FAIL'}] {label:32s} expect={'SEA' if exp else 'LAND'} got={'SEA' if got else 'LAND'}")
print(f"\n{len(cases)-bad}/{len(cases)} passed")
sys.exit(1 if bad else 0)
