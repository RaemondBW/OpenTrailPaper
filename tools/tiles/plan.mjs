#!/usr/bin/env node
// Plan a planet run: which Geofabrik extracts to build, in what (ownership)
// order, and how to pack them into GitHub Actions jobs.
//
//   node tools/tiles/plan.mjs --index index-v1.json --out plan.json
//        [--jobs 20] [--max-mb 1500] [--max-cells 600000] [--only id,id,…] [--sizes sizes.json]
//
// Start from the continents and replace any extract bigger than --max-mb, or
// whose polygon holds more than --max-cells H3 cells (Geofabrik polygons reach
// far offshore: australia-oceania covers ~3 M cells of Pacific), by its
// subregions (recursively), so one region fits a runner's 16 GB RAM, 14 GB
// disk and a few hours. Geofabrik also publishes overlapping
// aggregates (us-west, dach, alps, …); those are never used. Sizes come from
// HEAD requests (Content-Length), cached in --sizes when given.
//
// plan.json:
//   { regions: [{ id, url, mb, cells, cost, geometry }],   ownership order = sorted by id
//     jobs:    [{ name, regions: [id…], mb, cells, cost, needs: [job name…] }] }
// `needs`: the jobs whose strips the border phase of this job reads — every
// job holding a region within reach (0.5°) of one of this job's regions.
// Jobs are runs of regions in Geofabrik URL order, so neighbours mostly
// share a job.
import fs from "node:fs";
import { polygonToCells } from "h3-js";
import { Region } from "./poly.mjs";

// Aggregates that duplicate regions which are also published on their own.
const AGGREGATES = new Set([
  "us", "us-midwest", "us-northeast", "us-pacific", "us-south", "us-west",
  "alps", "britain-and-ireland", "dach", "great-britain",
  "south-africa-and-lesotho", "sea",
]);

function args() {
  const a = {};
  const v = process.argv.slice(2);
  for (let i = 0; i < v.length; i++) {
    if (!v[i].startsWith("--")) continue;
    const k = v[i].slice(2);
    if (i + 1 < v.length && !v[i + 1].startsWith("--")) a[k] = v[++i]; else a[k] = true;
  }
  return a;
}

async function headSize(url) {
  for (let attempt = 0; attempt < 4; attempt++) {
    try {
      const r = await fetch(url, { method: "HEAD", headers: { "User-Agent": "OpenTrailPaper tile planner (github.com/RaemondBW/OpenTrailPaper)" } });
      if (r.ok) return Number(r.headers.get("content-length")) / 1048576;
      if (r.status === 404) return null;
    } catch {}
    await new Promise((res) => setTimeout(res, 2000 * (attempt + 1)));
  }
  throw new Error(`HEAD ${url} failed`);
}

// Rough seconds on a 4-core runner (measured on Switzerland, Greenland,
// Wyoming): extract size drives parsing and dense cells, cell count drives
// the per-cell work (coastline, DEM). Only used to balance jobs.
const cost = (mb, cells) => 2 * (mb * 0.06 + cells * 0.0025);

function cellCount(geometry) {
  const polys = geometry.type === "Polygon" ? [geometry.coordinates] : geometry.coordinates;
  let n = 0;
  for (const p of polys) { try { n += polygonToCells(p, 6, true).length; } catch {} }
  return n;
}

export async function plan({ index, maxMb = 1500, maxCells = 600000, jobs = 20, only = null, sizes = {} }) {
  const byId = new Map(index.features.map((f) => [f.properties.id, f]));
  const children = new Map();
  for (const f of index.features) {
    const p = f.properties.parent;
    if (!p || AGGREGATES.has(f.properties.id)) continue;
    if (!children.has(p)) children.set(p, []);
    children.get(p).push(f.properties.id);
  }
  const size = async (id) => {
    if (sizes[id] === undefined) sizes[id] = await headSize(byId.get(id).properties.urls.pbf);
    return sizes[id];
  };
  const chosen = [];
  const cellsOf = new Map();
  const cells = (id) => { if (!cellsOf.has(id)) cellsOf.set(id, cellCount(byId.get(id).geometry)); return cellsOf.get(id); };
  const visit = async (id) => {
    const mb = await size(id);
    const kids = children.get(id) || [];
    if (mb != null && kids.length && (mb > maxMb || cells(id) > maxCells)) {
      // Fetch the children's sizes in parallel, then recurse.
      await Promise.all(kids.map(size));
      for (const k of kids.sort()) await visit(k);
    } else if (mb != null) chosen.push(id);
  };
  const roots = only ? only : index.features.filter((f) => !f.properties.parent).map((f) => f.properties.id).sort();
  for (const r of roots) await visit(r);
  chosen.sort();

  const regions = chosen.map((id) => {
    const f = byId.get(id);
    return { id, url: f.properties.urls.pbf, mb: Math.round(sizes[id] * 10) / 10, cells: cells(id), geometry: f.geometry };
  });

  // Pack in Geofabrik URL order (continent/country/subregion), so a job
  // holds neighbours and its border phase needs few other jobs' strips; cut
  // the sequence into runs of about equal size (time is ~linear in size).
  const n = Math.max(1, Math.min(jobs, regions.length));
  const seq = [...regions].sort((x, y) => (x.url < y.url ? -1 : 1));
  // At least 1: a tiny region (Monaco) rounded to 0, and an all-zero total
  // made every bin index NaN — no job at all.
  for (const r of seq) r.cost = Math.max(1, Math.round(cost(r.mb, r.cells)));
  const total = seq.reduce((t, r) => t + r.cost, 0);
  const bins = [];
  let acc = 0;
  for (const r of seq) {
    const want = Math.min(n - 1, Math.floor((acc + r.cost / 2) / (total / n)));
    while (bins.length <= want) bins.push({ name: `j${String(bins.length + 1).padStart(2, "0")}`, regions: [], mb: 0, cells: 0, cost: 0 });
    const b = bins[bins.length - 1];
    b.regions.push(r.id); b.mb += r.mb; b.cells += r.cells; b.cost += r.cost; acc += r.cost;
  }
  for (const b of bins) { b.regions.sort(); b.mb = Math.round(b.mb); }

  // Strip dependencies for the border phase.
  const poly = new Map(regions.map((r) => [r.id, new Region(r.geometry)]));
  const jobOf = new Map();
  for (const b of bins) for (const id of b.regions) jobOf.set(id, b.name);
  // Within reach: y's polygon touches x's polygon grown by 0.5° (tested
  // per edge bucket of x: a 0.5°-grown box around each 1° slice of x's
  // outline).
  const reachBoxes = new Map();
  const boxesOf = (x) => {
    if (reachBoxes.has(x)) return reachBoxes.get(x);
    const cells = new Set();
    for (const [la1, lo1, la2, lo2] of poly.get(x).edges) {
      for (const [la, lo] of [[la1, lo1], [la2, lo2], [(la1 + la2) / 2, (lo1 + lo2) / 2]]) cells.add(`${Math.floor(la)},${Math.floor(lo)}`);
    }
    const boxes = [...cells].map((c) => { const [la, lo] = c.split(",").map(Number); return { s: la - 0.5, w: lo - 0.5, n: la + 1.5, e: lo + 1.5 }; });
    reachBoxes.set(x, boxes);
    return boxes;
  };
  const near = (x, y) => {
    const a = poly.get(x).bbox, b = poly.get(y).bbox;
    if (a.s - 2 > b.n || b.s - 2 > a.n || a.w - 2 > b.e || b.w - 2 > a.e) return false;
    return boxesOf(x).some((q) => poly.get(y).intersectsRect(q.s, q.w, q.n, q.e));
  };
  for (const b of bins) {
    const needs = new Set([b.name]);
    for (const id of b.regions) for (const r of regions) if (!needs.has(jobOf.get(r.id)) && near(id, r.id)) needs.add(jobOf.get(r.id));
    b.needs = [...needs].sort();
  }
  return { regions, jobs: bins.filter((b) => b.regions.length) };
}

async function main() {
  const a = args();
  if (!a.index || !a.out) { console.error("usage: plan.mjs --index index-v1.json --out plan.json [--jobs N] [--max-mb MB] [--only id,…] [--sizes sizes.json]"); process.exit(2); }
  const index = JSON.parse(fs.readFileSync(a.index, "utf8"));
  const sizes = a.sizes && fs.existsSync(a.sizes) ? JSON.parse(fs.readFileSync(a.sizes, "utf8")) : {};
  const p = await plan({ index, maxMb: Number(a["max-mb"] || 1500), maxCells: Number(a["max-cells"] || 600000), jobs: Number(a.jobs || 20),
    only: a.only ? String(a.only).split(",") : null, sizes });
  if (a.sizes) fs.writeFileSync(a.sizes, JSON.stringify(sizes, null, 1));
  fs.writeFileSync(a.out, JSON.stringify(p));
  const total = p.regions.reduce((s, r) => s + r.mb, 0);
  const cells = p.regions.reduce((s, r) => s + r.cells, 0);
  console.error(`plan: ${p.regions.length} regions, ${(total / 1024).toFixed(1)} GB of extracts, ${(cells / 1e6).toFixed(2)} M cells, ${p.jobs.length} jobs:\n` +
    p.jobs.map((j) => `  ${j.name}: ${(j.mb / 1024).toFixed(1)} GB, ${(j.cells / 1e3).toFixed(0)} k cells, ~${Math.round(j.cost / 60)} min est., ` +
      `${j.regions.length} regions (${j.regions[0]} … ${j.regions[j.regions.length - 1]}), strips from ${j.needs.length} jobs`).join("\n"));
}

if (process.argv[1] && import.meta.url === `file://${(await import("node:path")).resolve(process.argv[1])}`) await main();
