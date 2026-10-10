#!/bin/sh
# Prove the border phase: build two neighbouring Geofabrik extracts the way
# the planet run does (interior per extract + strips, then the border phase),
# and compare every cell with a build from both extracts merged into one
# file — the same snapshot with no border at all.
#
#   tools/tiles/test/border_equivalence.sh <index-v1.json> <idA> <a.osm.pbf> <idB> <b.osm.pbf> [workdir]
# e.g. us/connecticut connecticut-latest.osm.pbf us/rhode-island rhode-island-latest.osm.pbf
# Needs node, osmium. Cells along the pair's OTHER borders lack the third
# extract in both builds, so they compare equal too.
set -e
cd "$(dirname "$0")/../../.."
INDEX=$1; A=$2; PA=$3; B=$4; PB=$5
OUT=${6:-$(mktemp -d)}; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
DEM=${DEM_CACHE:-$OUT/dem}
node -e '
  const [idx, a, b, out] = process.argv.slice(1);
  const f = JSON.parse(require("fs").readFileSync(idx, "utf8")).features;
  const g = (id) => { const x = f.find((x) => x.properties.id === id); return { id, url: x.properties.urls.pbf, geometry: x.geometry }; };
  require("fs").writeFileSync(out, JSON.stringify({ regions: [g(a), g(b)].sort((x, y) => (x.id < y.id ? -1 : 1)) }));
' "$INDEX" "$A" "$B" "$OUT/regions.json"
rm -rf "$OUT/multi" "$OUT/strips" "$OUT/ref"
for pair in "$A $PA" "$B $PB"; do
    set -- $pair
    node tools/tiles/build_region.mjs --regions "$OUT/regions.json" --region "$1" --pbf "$2" \
        --out "$OUT/multi" --strip-dir "$OUT/strips" --dem-cache "$DEM" --tmp "$OUT/tmp"
done
node tools/tiles/build_region.mjs --phase border --regions "$OUT/regions.json" --strip-dir "$OUT/strips" \
    --out "$OUT/multi" --dem-cache "$DEM"
osmium merge "$PA" "$PB" -O -o "$OUT/merged.osm.pbf" --no-progress
BOX=$(node -e '
  const r = JSON.parse(require("fs").readFileSync(process.argv[1], "utf8")).regions;
  let s = 90, w = 180, n = -90, e = -180;
  for (const x of r) for (const p of (x.geometry.type === "Polygon" ? [x.geometry.coordinates] : x.geometry.coordinates))
    for (const [lo, la] of p[0]) { s = Math.min(s, la); n = Math.max(n, la); w = Math.min(w, lo); e = Math.max(e, lo); }
  console.log([s - 1, w - 1, n + 1, e + 1].join(","));
' "$OUT/regions.json")
node tools/tiles/build_region.mjs --region reference --bbox "$BOX" --pbf "$OUT/merged.osm.pbf" \
    --out "$OUT/ref" --dem-cache "$DEM" --tmp "$OUT/tmp"
node tools/tiles/test/compare_fragments.mjs "$OUT/multi" "$OUT/ref"
