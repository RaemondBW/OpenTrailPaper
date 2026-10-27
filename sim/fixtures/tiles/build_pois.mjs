#!/usr/bin/env node
// Builds the synthetic cycling-POI fixture (sim/fixtures/tiles/pois/) for the
// device's POI tests (sim/tests/test_pois.py), over the real Hood River tiles.
//
//     node sim/fixtures/tiles/build_pois.mjs
//
// The CDN's own .poi files for these cells (next to the .ebm files) are a
// handful of toilets; a test needs every type, its flags, POIs either side of
// a cell edge, a crowd, and stops along the manifest's track — each at a
// known place. So the POIs are made up (as OSM nodes with the tags the
// builders read) and encoded by the real writer, docs/mapgen.js buildPoi,
// with each POI in the one H3 cell containing it, as tools/tiles/build_region
// and the apps do. Cells come from the apps' own H3 C code (tools/tiles/
// h3tool.c, built into a temp dir), so they are the ids the firmware computes.
//
// Output: pois/<set>/<h3>.poi and pois/pois.json, which lists per set the
// cells, every POI (name, type, flags, lat, lon, cell), and where to stand.
// Deterministic: the same input builds the same bytes.
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { buildPoi, poiOf } from "../../../docs/mapgen.js";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, "..", "..", "..");
const OUT = path.join(HERE, "pois");
const manifest = JSON.parse(fs.readFileSync(path.join(HERE, "manifest.json"), "utf8"));
const C = manifest.centre;

// --- H3, via the apps' C code ---------------------------------------------------------
const tool = path.join(fs.mkdtempSync(path.join(os.tmpdir(), "h3tool-")), "h3tool");
const built = spawnSync("sh", [path.join(ROOT, "tools", "tiles", "build_h3tool.sh"), tool], { stdio: "ignore" });
if (built.status !== 0) throw new Error("building h3tool failed");
const h3 = (mode, lines) => {
  const r = spawnSync(tool, [mode], { input: lines.join("\n") + "\n", encoding: "utf8" });
  if (r.status !== 0) throw new Error("h3tool " + mode + ": " + r.stderr);
  return r.stdout.trim().split("\n");
};
const cellsOf = (pts) => h3("cell", pts.map(([la, lo]) => `${la.toFixed(9)} ${lo.toFixed(9)}`));
const bboxOf = (cell) => h3("bbox", [cell])[0].split(" ").slice(1).map(Number);   // s w n e

// Metres east/north of a point, on the same flat projection the map uses.
const KY = 110540.0;
const at = (lat, lon, east, north) =>
  [lat + north / KY, lon + east / (111320.0 * Math.cos((lat * Math.PI) / 180))];

// --- the POIs, as OSM nodes -------------------------------------------------------------
const TAGS = {
  water: { amenity: "drinking_water" },
  water_fee: { amenity: "drinking_water", fee: "yes" },             // hollow icon
  toilets: { amenity: "toilets" },
  toilets_water: { amenity: "toilets", drinking_water: "yes" },
  repair: { amenity: "bicycle_repair_station" },
  repair_pump: { amenity: "bicycle_repair_station", "service:bicycle:pump": "yes",
                 "service:bicycle:tools": "yes" },
  shop: { shop: "bicycle", "service:bicycle:repair": "yes", "service:bicycle:retail": "yes" },
  private_water: { amenity: "drinking_water", access: "private" },  // dropped by the builder
};

let nextId = 1;
function node(name, kind, lat, lon) {
  return { name, kind, el: { type: "node", id: nextId++, lat, lon, tags: TAGS[kind] } };
}

const sets = {};

// "types": one of each kind around the centre, ±150 m E/W and ±200 m N/S
// (±75 / ±100 px at the map's default 2 m/px), clear of the rider marker and
// the chrome. The private tap is in the input only: the builder drops it.
{
  const [la, lo] = [C.lat, C.lon];
  sets.types = {
    stand: { lat: la, lon: lo },
    nodes: [
      node("water", "water", ...at(la, lo, -150, 200)),
      node("toilets", "toilets", ...at(la, lo, 150, 200)),
      node("repair_pump", "repair_pump", ...at(la, lo, -150, -200)),
      node("shop", "shop", ...at(la, lo, 150, -200)),
      node("water_fee", "water_fee", ...at(la, lo, 0, 330)),
      node("repair", "repair", ...at(la, lo, -150, -380)),
      node("toilets_water", "toilets_water", ...at(la, lo, 150, -380)),
      node("private_water", "private_water", ...at(la, lo, 100, 100)),
    ],
  };
}

// "edge": a water tap and a repair stand 60 m either side of the edge of the
// centre cell, due east of the centre; the map is opened just south of it.
{
  const inCentre = (e) => cellsOf([at(C.lat, C.lon, e, 0)])[0] === C.h3;
  let lo = 0, hi = 50;
  while (inCentre(hi)) { lo = hi; hi += 50; }
  while (hi - lo > 0.5) { const m = (lo + hi) / 2; if (inCentre(m)) lo = m; else hi = m; }
  const edge = at(C.lat, C.lon, lo, 0);
  const west = node("water_west", "water", ...at(edge[0], edge[1], -60, 0));
  const east = node("repair_east", "repair_pump", ...at(edge[0], edge[1], 60, 0));
  const [cw, ce] = cellsOf([[west.el.lat, west.el.lon], [east.el.lat, east.el.lon]]);
  if (cw !== C.h3 || ce === C.h3) throw new Error("edge POIs not either side of the edge");
  // Stand 150 m south of the edge: the pair is then 75 px above the rider
  // marker, which hides any icon within 36 px of it.
  const stand = at(edge[0], edge[1], 0, -150);
  sets.edge = { stand: { lat: stand[0], lon: stand[1] }, edge: { lat: edge[0], lon: edge[1] }, nodes: [west, east] };
}

// "crowd": 300 toilets on a 20 m grid south of the centre (more than the
// firmware's 256-POI frame budget, MAX_POIS), then — later in the file, which
// buildPoi sorts south to north — a water tap and a repair stand north of the
// rider.
{
  const [la, lo] = [C.lat, C.lon];
  const nodes = [];
  for (let r = 0; r < 15; r++)
    for (let c = 0; c < 20; c++)
      nodes.push(node(`t${r}_${c}`, "toilets", ...at(la, lo, -190 + 20 * c, -80 - 20 * r)));
  nodes.push(node("water_north", "water", ...at(la, lo, -120, 250)));
  nodes.push(node("repair_north", "repair_pump", ...at(la, lo, 120, 250)));
  sets.crowd = { stand: { lat: la, lon: lo }, nodes };
}

// "ride": a water tap every 1 km along the manifest's track (10 km west to
// east through the centre), 100 m north of it, and one ON the track, 300 m
// east of the centre, under a route drawn along it.
{
  const [[la0, lo0], [la1, lo1]] = [manifest.track.from, manifest.track.to];
  const nodes = [];
  for (let k = 1; k < 10; k++) {
    const t = k / 10;
    nodes.push(node(`water_km${k}`, "water", ...at(la0 + (la1 - la0) * t, lo0 + (lo1 - lo0) * t, 0, 100)));
  }
  const t = 0.5 + 300 / 10000;
  nodes.push(node("water_on_route", "water", la0 + (la1 - la0) * t, lo0 + (lo1 - lo0) * t));
  sets.ride = { stand: { lat: la0, lon: lo0 }, nodes };
}

// --- encode -------------------------------------------------------------------------------
fs.rmSync(OUT, { recursive: true, force: true });
fs.mkdirSync(OUT, { recursive: true });
const index = {
  generator: "sim/fixtures/tiles/build_pois.mjs (docs/mapgen.js buildPoi)",
  types: { water: 1, toilets: 2, repair: 3, bike_shop: 4 },
  sets: {},
};
for (const [name, set] of Object.entries(sets)) {
  const json = { elements: set.nodes.map((n) => n.el) };
  const cells = cellsOf(set.nodes.map((n) => [n.el.lat, n.el.lon]));
  const byCell = [...new Set(cells)].sort();
  fs.mkdirSync(path.join(OUT, name));
  const out = { stand: set.stand, ...(set.edge ? { edge: set.edge } : {}), cells: {}, pois: [] };
  for (const cell of byCell) {
    const [s, w, n, e] = bboxOf(cell);
    // The H3 test, as build_region passes it: each POI in exactly one file.
    const mine = new Set(set.nodes.filter((_, i) => cells[i] === cell).map((n) => n.el.id));
    const lookup = new Map(set.nodes.map((n) => [`${n.el.lat},${n.el.lon}`, n.el.id]));
    const bytes = buildPoi(json, { s, w, n, e, cell, contains: (la, lo) => mine.has(lookup.get(`${la},${lo}`)) });
    fs.writeFileSync(path.join(OUT, name, cell + ".poi"), bytes);
    const count = bytes[6] | (bytes[7] << 8);
    out.cells[cell] = { file: `${name}/${cell}.poi`, count, size: bytes.length };
  }
  set.nodes.forEach((n, i) => {
    const [type, flags] = poiOf(n.el.tags) || [0, 0];   // 0: the builder drops it
    out.pois.push({ name: n.name, kind: n.kind, type, flags, lat: n.el.lat, lon: n.el.lon, cell: cells[i] });
  });
  index.sets[name] = out;
}
// One POI per line: the crowd alone is 302 of them.
const text = JSON.stringify(index, (k, v) => (k === "pois" ? v.map((p) => JSON.stringify(p)) : v), 1)
  .replace(/"(\{[^\n]*\})"/g, (_, o) => JSON.parse(`"${o}"`));
fs.writeFileSync(path.join(OUT, "pois.json"), text + "\n");
for (const [name, s] of Object.entries(index.sets))
  console.log(`${name}: ${s.pois.length} POIs in`, Object.entries(s.cells).map(([c, v]) => `${c} (${v.count})`).join(", "));
