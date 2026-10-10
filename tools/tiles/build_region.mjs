#!/usr/bin/env node
// Build the pre-built map tiles for one OSM extract.
//
//   node tools/tiles/build_region.mjs --pbf <extract.osm.pbf> --region <name> --out <dir>
//        [--regions <regions.json>]     ownership: the planner's ordered region list
//        [--bbox s,w,n,e]               ...or own every cell whose fetch box is in this box
//        [--prev <fragment.json>]       last run's fragment: only changed files are listed
//        [--dem-cache <dir>] [--no-elevation] [--workers N] [--limit N] [--cells id,id,…]
//
// For every H3 res-6 cell this region OWNS (see cellsFor), writes
//   <out>/v1/<id[0:6]>/<id>.ebm   the tile, byte-identical to what the apps build
//                                 when that hex is downloaded alone (absent when
//                                 the app would drop it as empty)
//   <out>/v1/<id[0:6]>/<id>.poi   only when the cell has POIs (an empty .poi is
//                                 implied by the index: poiSize 0)
//   <out>/v1/regions/<region>.json  the region fragment / manifest: every owned
//                                 cell -> [ebmSize, ebmHash, poiSize, poiHash]
//   <out>/upload.txt              object keys that are new or changed vs --prev
//   (Deletions are global — a cell can move to another region — so
//   merge_index.mjs works them out from all old and new fragments.)
//
// id[0:6] of a res-6 H3 id is exactly its res-3 ancestor (resolution, base cell
// and digits 1-3), so a directory is one res-3 cell (<= 343 tiles) and
// merge_index.mjs writes one v1/<id[0:6]>/index.json per directory.
//
// Each tile is built by answering the apps' Overpass queries from the extract
// (overpass.mjs) and running the apps' own builder on that JSON: docs/mapgen.js
// for roads and POIs, apptile.mjs for ELV1/WTR2/PRK2. Elevation comes from
// Copernicus GLO-90 (dem.mjs), sampled the way Open-Meteo answers the apps.
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import crypto from "node:crypto";
import { spawnSync } from "node:child_process";
import { Worker, isMainThread, parentPort, workerData } from "node:worker_threads";
import { fileURLToPath } from "node:url";
import { polygonToCells } from "h3-js";
import { buildPoi } from "../../docs/mapgen.js";
import { loadOsm } from "./osmstore.mjs";
import { OverpassEmu, poiResponse } from "./overpass.mjs";
import { assembleCoastlineFast } from "./coastline.mjs";
import { buildAppTile, seaBox, mapBox, seaRingsFor, elevationSamplePoints, elevationValue } from "./apptile.mjs";
import { Dem } from "./dem.mjs";
import { Region, bboxGeometry } from "./poly.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
export const LAYOUT_VERSION = "v1";
export const tileKey = (id, ext) => `${LAYOUT_VERSION}/${id.slice(0, 6)}/${id}${ext}`;
const hash = (b) => crypto.createHash("sha256").update(b).digest("hex").slice(0, 16);

// ---- H3 via the apps' C (h3tool.c) -----------------------------------------
function h3tool() {
  const bin = process.env.H3TOOL || path.join(HERE, ".bin", "h3tool");
  if (!fs.existsSync(bin)) spawnSync(path.join(HERE, "build_h3tool.sh"), [bin], { stdio: "inherit" });
  return bin;
}
function h3batch(mode, lines) {
  const out = [];
  for (let i = 0; i < lines.length; i += 200000) {
    const r = spawnSync(h3tool(), [mode], { input: lines.slice(i, i + 200000).join("\n") + "\n", maxBuffer: 1 << 30 });
    if (r.status !== 0) throw new Error(`h3tool ${mode} failed: ${r.stderr}`);
    out.push(...r.stdout.toString().trim().split("\n"));
  }
  return out;
}
export function cellBboxes(ids) {
  return h3batch("bbox", ids).map((l) => {
    const f = l.split(" ");
    return { id: f[0], s: Number(f[1]), w: Number(f[2]), n: Number(f[3]), e: Number(f[4]) };
  });
}
export function cellsAt(points) {
  return h3batch("cell", points.map(([la, lo]) => `${la} ${lo}`));
}

// ---- ownership ----------------------------------------------------------------
// A cell is built by the FIRST region (in the planner's order) whose polygon
// contains the cell's whole Overpass fetch box (mapBox), so every way the app
// would have fetched is in that extract. Cells no region contains are not
// built; the apps fall back to Overpass for them.
function cellsFor(own, earlier, log) {
  const cand = new Set();
  const polys = own.polys;
  for (const poly of polys) {
    // polygonToCells: cells whose centre is inside — a superset of the cells
    // whose fetch box is inside.
    for (const c of polygonToCells(poly, 6, true)) cand.add(c);
  }
  const boxes = cellBboxes([...cand].sort());
  const owned = [];
  let notInside = 0, earlierOwns = 0;
  for (const t of boxes) {
    const B = mapBox(t);
    if (!own.containsRect(B.s, B.w, B.n, B.e)) { notInside++; continue; }
    if (earlier.some((r) => r.containsRect(B.s, B.w, B.n, B.e))) { earlierOwns++; continue; }
    owned.push(t);
  }
  log(`cells: ${cand.size} with centre inside, ${owned.length} owned, ${notInside} fetch box not inside, ${earlierOwns} owned by an earlier region`);
  return owned;
}

// ---- per-cell build (runs in workers) -------------------------------------------
function makeBuilder(store, emuData, demOpts, elevation) {
  const emu = new OverpassEmu(store, emuData);
  const dem = elevation ? new Dem(demOpts) : null;
  return async function build(t, pois) {
    const t0 = performance.now();
    const json = emu.mapResponse(mapBox(t));
    const P = seaBox(t);
    const { coastWays, nodes } = emu.coast(P);
    const rings = coastWays.length ? seaRingsFor(assembleCoastlineFast(coastWays, nodes), t) : [];
    let grid = null;
    if (dem) {
      grid = [];
      for (const [la, lo] of elevationSamplePoints(t.s, t.w, t.n, t.e)) grid.push(elevationValue(await dem.at(la, lo)));
    }
    const ebm = buildAppTile(json, rings, grid, t);
    const poi = buildPoi(poiResponse(pois), { s: t.s, w: t.w, n: t.n, e: t.e, cell: t.id, contains: () => true });
    return { ebm, poi, ms: performance.now() - t0, ways: json.elements.filter((e) => e.type === "way").length, dem: dem?.stats };
  };
}

if (!isMainThread) {
  const { store, emuData, demOpts, elevation } = workerData;
  const build = makeBuilder(store, emuData, demOpts, elevation);
  parentPort.on("message", async ({ t, pois }) => {
    try {
      const r = await build(t, pois);
      parentPort.postMessage({ id: t.id, ...r }, r.ebm ? [r.ebm.buffer, r.poi.buffer] : [r.poi.buffer]);
    } catch (e) {
      parentPort.postMessage({ id: t.id, error: e.stack || String(e) });
    }
  });
} else if (process.argv[1] && fileURLToPath(import.meta.url) === path.resolve(process.argv[1])) {
  await main();
}

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

// Copy a typed array into shared memory so workers read it without a copy.
function share(ta) {
  const s = new ta.constructor(new SharedArrayBuffer(ta.byteLength));
  s.set(ta);
  return s;
}

async function main() {
  const a = args();
  const started = Date.now();
  const log = (m) => console.error(`[${((Date.now() - started) / 1000).toFixed(1)}s] ${m}`);
  if (!a.pbf || !a.region || !a.out) {
    console.error("usage: build_region.mjs --pbf <file> --region <name> --out <dir> [--regions regions.json | --bbox s,w,n,e] [--prev fragment.json]");
    process.exit(2);
  }
  const out = path.resolve(a.out);
  const tmp = a.tmp ? path.resolve(a.tmp) : fs.mkdtempSync(path.join(os.tmpdir(), "otp-tiles-"));

  // Ownership.
  let own, earlier = [];
  if (a.regions) {
    const regions = JSON.parse(fs.readFileSync(a.regions, "utf8")).regions;
    const k = regions.findIndex((r) => r.id === a.region);
    if (k < 0) throw new Error(`region ${a.region} not in ${a.regions}`);
    own = new Region(regions[k].geometry);
    for (const r of regions.slice(0, k)) {
      const g = new Region(r.geometry);
      const b = g.bbox, o = own.bbox;
      if (b.e < o.w || b.w > o.e || b.n < o.s || b.s > o.n) continue;
      earlier.push(g);
    }
  } else if (a.bbox) {
    const [s, w, n, e] = a.bbox.split(",").map(Number);
    own = new Region(bboxGeometry(s, w, n, e));
  } else throw new Error("need --regions or --bbox");
  let cells = cellsFor(own, earlier, log);
  if (a.cells) { const want = new Set(a.cells.split(",")); cells = cells.filter((t) => want.has(t.id)); }
  if (a.limit) cells = cells.slice(0, Number(a.limit));

  // OSM data.
  const osmTs = spawnSync("osmium", ["fileinfo", "-g", "header.option.osmosis_replication_timestamp", a.pbf]).stdout.toString().trim() || null;
  const store = await loadOsm(path.resolve(a.pbf), { tmpDir: tmp, log });
  const emu = new OverpassEmu(store);
  log(`spatial index built`);

  // POIs -> cells, once (MapsView.sendPois: byCell[H3 id of the POI]).
  const poiCells = cellsAt(store.pois.map((p) => [p.lat, p.lon]));
  const poisByCell = new Map();
  store.pois.forEach((p, i) => {
    const c = poiCells[i];
    let l = poisByCell.get(c); if (!l) poisByCell.set(c, (l = [])); l.push(p);
  });

  const prev = a.prev && fs.existsSync(a.prev) ? JSON.parse(fs.readFileSync(a.prev, "utf8")) : null;
  const prevCells = prev?.cells || {};
  const elevation = !a["no-elevation"];
  const demOpts = { cacheDir: a["dem-cache"] ? path.resolve(a["dem-cache"]) : path.join(tmp, "dem"), maxTiles: 24 };
  if (elevation) fs.mkdirSync(demOpts.cacheDir, { recursive: true });

  // Workers share the typed arrays; tags/POIs are cloned (small).
  const nWorkers = Math.max(1, Number(a.workers || os.availableParallelism?.() || os.cpus().length));
  const shared = {
    ways: Object.fromEntries(Object.entries(store.ways).map(([k, v]) => [k, share(v)])),
    refs: Object.fromEntries(Object.entries(store.refs).map(([k, v]) => [k, share(v)])),
    tags: store.tags, pois: [], sorted: store.sorted,
  };
  const emuData = emu.exportShared(share);

  // Pre-fetch the DEM tiles once in the main thread so workers read the cache.
  if (elevation) {
    const dem = new Dem(demOpts);
    const need = new Set();
    for (const t of cells) for (let la = Math.floor(t.s); la <= Math.floor(t.n); la++) for (let lo = Math.floor(t.w); lo <= Math.floor(t.e); lo++) need.add(`${la},${lo}`);
    await Promise.all([...need].map((k) => { const [la, lo] = k.split(",").map(Number); return dem.at(la + 0.5, lo + 0.5); }));
    log(`DEM: ${need.size} 1° tiles needed, ${dem.stats.downloads} downloaded (${(dem.stats.bytes / 1048576).toFixed(0)} MB), ${dem.stats.missing} sea/absent`);
  }

  const frag = {
    version: LAYOUT_VERSION, region: a.region, built: new Date().toISOString(), osm: osmTs,
    source: a["source-url"] || path.basename(a.pbf), cells: {},
  };
  const upload = [];
  const stats = { cells: 0, empty: 0, ebmBytes: 0, poiBytes: 0, pois: 0, withPois: 0, changed: 0, ms: 0, slowest: null };
  const writeCell = (t, ebm, poi, ms) => {
    const poiCount = poi[6] | (poi[7] << 8);
    // ebm null: the app would drop this tile as empty (open sea with no
    // coast in reach, an empty desert). Recorded as size 0 so the apps know
    // the hex was built and need not ask Overpass for it.
    const rec = [ebm ? ebm.length : 0, ebm ? hash(ebm) : "", poiCount ? poi.length : 0, poiCount ? hash(poi) : ""];
    frag.cells[t.id] = rec;
    const old = prevCells[t.id];
    const ek = tileKey(t.id, ".ebm"), pk = tileKey(t.id, ".poi");
    fs.mkdirSync(path.join(out, path.dirname(ek)), { recursive: true });
    if (ebm && (!old || old[1] !== rec[1])) { fs.writeFileSync(path.join(out, ek), ebm); upload.push(ek); }
    if (poiCount && (!old || old[3] !== rec[3])) { fs.writeFileSync(path.join(out, pk), poi); upload.push(pk); }
    if (!old || old[1] !== rec[1] || old[3] !== rec[3]) stats.changed++;
    stats.cells++; stats.ms += ms;
    if (ebm) stats.ebmBytes += ebm.length; else stats.empty++;
    if (poiCount) { stats.withPois++; stats.poiBytes += poi.length; stats.pois += poiCount; }
    if (!stats.slowest || ms > stats.slowest[1]) stats.slowest = [t.id, Math.round(ms)];
  };

  const workers = Array.from({ length: Math.min(nWorkers, cells.length) }, () =>
    new Worker(fileURLToPath(import.meta.url), { workerData: { store: shared, emuData, demOpts, elevation } }));
  const byId = new Map(cells.map((t) => [t.id, t]));
  let next = 0, done = 0, lastLog = Date.now();
  await Promise.all(workers.map((wk) => new Promise((resolve, reject) => {
    const feed = () => {
      if (next >= cells.length) { wk.terminate(); resolve(); return; }
      const t = cells[next++];
      wk.postMessage({ t, pois: poisByCell.get(t.id) || [] });
    };
    wk.on("message", (m) => {
      if (m.error) { reject(new Error(`${m.id}: ${m.error}`)); return; }
      const t = byId.get(m.id);
      writeCell(t, m.ebm ? new Uint8Array(m.ebm.buffer ?? m.ebm) : null, new Uint8Array(m.poi.buffer ?? m.poi), m.ms);
      done++;
      if (Date.now() - lastLog > 30000) { lastLog = Date.now(); log(`${done}/${cells.length} cells`); }
      feed();
    });
    wk.on("error", reject);
    feed();
  })));

  frag.stats = { ...stats, seconds: (Date.now() - started) / 1000 };
  const fk = `${LAYOUT_VERSION}/regions/${a.region.replace(/\//g, "_")}.json`;
  fs.mkdirSync(path.join(out, path.dirname(fk)), { recursive: true });
  fs.writeFileSync(path.join(out, fk), JSON.stringify(frag));
  const lines = (l) => l.join("\n") + (l.length ? "\n" : "");
  fs.writeFileSync(path.join(out, "upload.txt"), lines(upload));
  log(`done: ${stats.cells} cells (${stats.empty} empty), ${stats.changed} changed, ${(stats.ebmBytes / 1048576).toFixed(1)} MB .ebm, ` +
      `${stats.withPois} .poi (${stats.pois} POIs), ${(stats.ms / 1000).toFixed(1)} s cell time summed, slowest ${stats.slowest?.join(" ")} ms`);
  if (!a.tmp) fs.rmSync(tmp, { recursive: true, force: true });
}
