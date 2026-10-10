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
import { buildPoi } from "../../docs/mapgen.js";
import { loadOsm } from "./osmstore.mjs";
import { OverpassEmu, poiResponse } from "./overpass.mjs";
import { assembleCoastlineFast } from "./coastline.mjs";
import { buildAppTile, seaBox, mapBox, seaRingsFor, elevationSamplePoints, elevationValue } from "./apptile.mjs";
import { Dem } from "./dem.mjs";
import { bboxGeometry } from "./poly.mjs";
import { loadRegions, cellsNear } from "./regions.mjs";
import { writeStrip, readStripHeader, loadStrips } from "./strip.mjs";

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
    return { ebm, poi, ms: performance.now() - t0, ways: json.elements.filter((e) => e.type === "way").length, coast: coastWays.length };
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

// Build `cells` from `store` on a worker pool; onCell(t, ebm|null, poi, m).
async function runCells(cells, store, poisByCell, { workers: nWorkers, demOpts, elevation }, onCell, log) {
  if (!cells.length) return;
  const emu = new OverpassEmu(store);
  const shared = {
    ways: Object.fromEntries(Object.entries(store.ways).map(([k, v]) => [k, share(v)])),
    refs: Object.fromEntries(Object.entries(store.refs).map(([k, v]) => [k, share(v)])),
    tags: store.tags, pois: [], sorted: store.sorted,
  };
  const emuData = emu.exportShared(share);
  // Fetch the DEM tiles once, here, so the workers only read the cache.
  if (elevation) {
    const dem = new Dem(demOpts);
    const need = new Set();
    for (const t of cells) for (let la = Math.floor(t.s); la <= Math.floor(t.n); la++) for (let lo = Math.floor(t.w); lo <= Math.floor(t.e); lo++) need.add(`${la},${lo}`);
    const keys = [...need];
    for (let i = 0; i < keys.length; i += 8) {
      await Promise.all(keys.slice(i, i + 8).map((k) => { const [la, lo] = k.split(",").map(Number); return dem.at(la + 0.5, lo + 0.5); }));
    }
    log(`DEM: ${need.size} 1° tiles needed, ${dem.stats.downloads} downloaded (${(dem.stats.bytes / 1048576).toFixed(0)} MB), ${dem.stats.missing} sea/absent`);
  }
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
      onCell(byId.get(m.id), m.ebm ? new Uint8Array(m.ebm.buffer ?? m.ebm) : null, new Uint8Array(m.poi.buffer ?? m.poi), m);
      done++;
      if (Date.now() - lastLog > 60000) { lastLog = Date.now(); log(`${done}/${cells.length} cells`); }
      feed();
    });
    wk.on("error", reject);
    feed();
  })));
}

function groupPois(pois) {
  const cells = cellsAt(pois.map((p) => [p.lat, p.lon]));
  const by = new Map();
  pois.forEach((p, i) => { const c = cells[i]; let l = by.get(c); if (!l) by.set(c, (l = [])); l.push(p); });
  return by;
}

export const fragmentName = (region, phase) => region.replace(/\//g, "_") + (phase === "border" ? ".border" : "");

// One fragment (v1/regions/<name>.json) and the tile files it writes.
class Fragment {
  constructor({ region, phase, out, prevDir }) {
    this.name = fragmentName(region, phase);
    this.out = out;
    const pf = prevDir && path.join(prevDir, this.name + ".json");
    this.prev = pf && fs.existsSync(pf) ? JSON.parse(fs.readFileSync(pf, "utf8")).cells || {} : {};
    this.f = { version: LAYOUT_VERSION, region, phase, built: new Date().toISOString(), osm: null, source: null, cells: {} };
    this.upload = [];
    this.stats = { cells: 0, empty: 0, deferred: 0, ebmBytes: 0, poiBytes: 0, pois: 0, withPois: 0, changed: 0, ms: 0, slowest: null };
  }
  write(t, ebm, poi, ms) {
    const { stats, out } = this;
    const poiCount = poi[6] | (poi[7] << 8);
    // ebm null: the app would drop this tile as empty. Recorded as size 0 so
    // the apps know the hex was built and need not ask Overpass for it.
    const rec = [ebm ? ebm.length : 0, ebm ? hash(ebm) : "", poiCount ? poi.length : 0, poiCount ? hash(poi) : ""];
    this.f.cells[t.id] = rec;
    const old = this.prev[t.id];
    const ek = tileKey(t.id, ".ebm"), pk = tileKey(t.id, ".poi");
    fs.mkdirSync(path.join(out, path.dirname(ek)), { recursive: true });
    if (ebm && (!old || old[1] !== rec[1])) { fs.writeFileSync(path.join(out, ek), ebm); this.upload.push(ek); }
    if (poiCount && (!old || old[3] !== rec[3])) { fs.writeFileSync(path.join(out, pk), poi); this.upload.push(pk); }
    if (!old || old[1] !== rec[1] || old[3] !== rec[3]) stats.changed++;
    stats.cells++; stats.ms += ms;
    if (ebm) stats.ebmBytes += ebm.length; else stats.empty++;
    if (poiCount) { stats.withPois++; stats.poiBytes += poi.length; stats.pois += poiCount; }
    if (!stats.slowest || ms > stats.slowest[1]) stats.slowest = [t.id, Math.round(ms)];
  }
  save(seconds, log) {
    const { stats, out } = this;
    this.f.stats = { ...stats, seconds };
    const fk = `${LAYOUT_VERSION}/regions/${this.name}.json`;
    fs.mkdirSync(path.join(out, path.dirname(fk)), { recursive: true });
    fs.writeFileSync(path.join(out, fk), JSON.stringify(this.f));
    log(`${this.name}: ${stats.cells} cells (${stats.empty} empty${this.f.phase === "interior" ? `, ${stats.deferred} deferred to the border phase` : ""}), ` +
        `${stats.changed} changed, ${(stats.ebmBytes / 1048576).toFixed(1)} MB .ebm, ` +
        `${stats.withPois} .poi (${stats.pois} POIs), ${(stats.ms / 1000).toFixed(1)} s cell time summed, slowest ${stats.slowest?.join(" ")} ms`);
    return this.upload;
  }
}

const USAGE = `usage:
  interior phase — the cells this extract can build alone, and its strip:
    build_region.mjs --region <id> --pbf <extract.osm.pbf> --out <dir>
                     (--regions regions.json | --bbox s,w,n,e) [--strip-dir <dir>]
  border phase — the border/deferred cells of the given regions, from strips:
    build_region.mjs --phase border --strip-dir <dir> --out <dir> [--todo <region,…>]
  common: [--prev-dir <old fragments>] [--dem-cache <dir>] [--no-elevation]
          [--workers N] [--cells id,…] [--tmp <dir>] [--source-url <url>]
  <out>/upload.txt lists the object keys that are new or changed vs --prev-dir.`;

async function main() {
  const a = args();
  const started = Date.now();
  const log = (m) => console.error(`[${((Date.now() - started) / 1000).toFixed(1)}s] ${m}`);
  const phase = a.phase || "interior";
  if (!a.out || (phase === "interior" && (!a.pbf || !a.region || !(a.regions || a.bbox))) ||
      (phase === "border" && !a["strip-dir"])) { console.error(USAGE); process.exit(2); }
  const out = path.resolve(a.out);
  const tmp = a.tmp ? path.resolve(a.tmp) : fs.mkdtempSync(path.join(os.tmpdir(), "otp-tiles-"));
  const elevation = !a["no-elevation"];
  const demOpts = { cacheDir: a["dem-cache"] ? path.resolve(a["dem-cache"]) : path.join(tmp, "dem"), maxTiles: 24 };
  if (elevation) fs.mkdirSync(demOpts.cacheDir, { recursive: true });
  const workers = Math.max(1, Number(a.workers || os.availableParallelism?.() || os.cpus().length));
  const stripDir = a["strip-dir"] ? path.resolve(a["strip-dir"]) : null;
  if (stripDir) fs.mkdirSync(stripDir, { recursive: true });
  const prevDir = a["prev-dir"] ? path.resolve(a["prev-dir"]) : null;
  const frags = [];
  const opts = { workers, demOpts, elevation };
  const want = a.cells ? new Set(a.cells.split(",")) : null;

  if (phase === "interior") {
    const regions = a.regions ? loadRegions(a.regions)
      : loadRegions({ regions: [{ id: a.region, geometry: bboxGeometry(...a.bbox.split(",").map(Number)) }] });
    const k = regions.findIndex((r) => r.id === a.region);
    if (k < 0) throw new Error(`region ${a.region} not in ${a.regions}`);
    const R = regions[k];
    const near = cellsNear(regions, k, cellBboxes);
    let interior = near.filter((c) => c.owner === k && c.interior);
    const border = near.filter((c) => c.owner === k && !c.interior);
    if (want) interior = interior.filter((t) => want.has(t.id));
    log(`cells: ${near.length} reach this extract; owns ${interior.length} interior + ${border.length} border; ` +
        `${near.filter((c) => c.owner < 0).length} unowned`);
    const frag = new Fragment({ region: a.region, phase, out, prevDir });
    frags.push(frag);
    const stats = frag.stats;

    frag.f.osm = spawnSync("osmium", ["fileinfo", "-g", "header.option.osmosis_replication_timestamp", a.pbf]).stdout.toString().trim() || null;
    frag.f.source = a["source-url"] || path.basename(a.pbf);
    const store = await loadOsm(path.resolve(a.pbf), { tmpDir: tmp, log });
    const poisByCell = groupPois(store.pois);

    // Deferred cells (regions.mjs): the coastline box reaches another
    // extract and this one sees coastline there — or nothing at all, which
    // may be open water whose coast is the neighbour's. Built in the border
    // phase with every extract's coastline.
    const deferred = [];
    await runCells(interior, store, poisByCell, opts, (t, ebm, poi, m) => {
      if (t.seaOut && (m.coast > 0 || m.ways === 0)) { deferred.push(t); stats.deferred++; return; }
      frag.write(t, ebm, poi, m.ms);
    }, log);

    if (stripDir) {
      const emu = new OverpassEmu(store);
      const wayIdx = new Set();
      const stripCells = new Set();
      for (const c of near) {
        if (c.owner < 0) continue;
        const B = mapBox(c), S = seaBox(c);
        if (!c.interior) {
          stripCells.add(c.id);
          if (R.poly.intersectsRect(B.s, B.w, B.n, B.e)) for (const i of emu.mapWayIdx(B)) wayIdx.add(i);
          for (const i of emu.coastWayIdx(S)) wayIdx.add(i);
        } else if (c.owner !== k && c.seaOut) {
          // The owner may defer it (see above) and then needs our coastline.
          for (const i of emu.coastWayIdx(S)) wayIdx.add(i);
        }
      }
      for (const t of deferred) {
        stripCells.add(t.id);
        for (const i of emu.mapWayIdx(mapBox(t))) wayIdx.add(i);
        for (const i of emu.coastWayIdx(seaBox(t))) wayIdx.add(i);
      }
      const pois = [];
      for (const id of stripCells) for (const p of poisByCell.get(id) || []) pois.push(p);
      const file = path.join(stripDir, `${a.region.replace(/\//g, "_")}.strip.ndjson.gz`);
      const r = await writeStrip(file, { store, wayIdx, pois, region: a.region, osm: frag.f.osm });
      const todo = [...border, ...deferred].map(({ id, s, w, n, e }) => ({ id, s, w, n, e })).sort((x, y) => (x.id < y.id ? -1 : 1));
      fs.writeFileSync(path.join(stripDir, `${a.region.replace(/\//g, "_")}.todo.json`),
        JSON.stringify({ region: a.region, k, cells: todo }));
      log(`strip: ${r.ways} ways, ${r.pois} POIs, ${(fs.statSync(file).size / 1048576).toFixed(1)} MB; ` +
          `todo for the border phase: ${border.length} border + ${deferred.length} deferred cells`);
    }
  } else {
    // Border phase: the todo cells of the given regions (default: every
    // todo file in the strip dir), from every strip that reaches them. One
    // fragment per owner region (<region>.border), so it stays put however
    // the regions are grouped into jobs.
    const files = fs.readdirSync(stripDir);
    const only = a.todo ? new Set(String(a.todo).split(",").map((r) => r.replace(/\//g, "_"))) : null;
    let cells = [];
    const fragOf = new Map();
    for (const f of files.filter((f) => f.endsWith(".todo.json")).sort()) {
      if (only && !only.has(f.slice(0, -".todo.json".length))) continue;
      const todo = JSON.parse(fs.readFileSync(path.join(stripDir, f), "utf8"));
      const frag = new Fragment({ region: todo.region, phase, out, prevDir });
      frags.push(frag);
      for (const t of todo.cells) { if (!want || want.has(t.id)) { cells.push(t); fragOf.set(t.id, frag); } }
    }
    cells.sort((x, y) => (x.id < y.id ? -1 : 1));
    const hits = (b, t) => { const S = seaBox(t); return !(b.n < S.s || b.s > S.n || b.e < S.w || b.w > S.e); };
    const strips = [];
    for (const f of files.filter((f) => f.endsWith(".strip.ndjson.gz"))) {
      const h = await readStripHeader(path.join(stripDir, f));
      if (h && h.ways + h.pois > 0 && cells.some((t) => hits(h.bbox, t))) strips.push({ f: path.join(stripDir, f), h });
    }
    // Region order (regions.json) decides version ties, if given.
    const order = a.regions ? loadRegions(a.regions).map((r) => r.id) : [];
    const rank = (id) => { const i = order.indexOf(id); return i < 0 ? order.length : i; };
    strips.sort((x, y) => rank(x.h.region) - rank(y.h.region) || (x.h.region < y.h.region ? -1 : 1));
    log(`border: ${cells.length} cells of ${frags.length} regions, ${strips.length} strips (${strips.map((s) => s.h.region).join(", ")})`);
    const { store, headers } = await loadStrips(strips.map((s) => s.f), log);
    for (const frag of frags) {
      frag.f.osm = headers.map((h) => h.osm).filter(Boolean).sort()[0] || null;
      frag.f.source = headers.map((h) => h.region).join(",");
    }
    await runCells(cells, store, groupPois(store.pois), opts, (t, ebm, poi, m) => fragOf.get(t.id).write(t, ebm, poi, m.ms), log);
  }

  const upload = [];
  for (const frag of frags) upload.push(...frag.save((Date.now() - started) / 1000, log));
  fs.writeFileSync(path.join(out, "upload.txt"), upload.join("\n") + (upload.length ? "\n" : ""));
  if (!a.tmp) fs.rmSync(tmp, { recursive: true, force: true });
}

if (isMainThread && process.argv[1] && fileURLToPath(import.meta.url) === path.resolve(process.argv[1])) await main();
