#!/usr/bin/env node
// Merge the per-region fragments into the global lookup the apps read, and
// work out what to delete.
//
//   node tools/tiles/merge_index.mjs --new <dir> --old <dir> --out <dir>
//        [--regions regions.json]
//
//   --new  this run's fragments (v1/regions/<region>.json from each job)
//   --old  the fragments currently published (last run), may be empty
//   --out  writes:
//     v1/<g>/index.json   one per res-3 group g = id[0:6] whose contents
//                         changed (or every group on a first run)
//     v1/meta.json        when the run happened, per-region OSM timestamps
//     v1/regions/*.json   the fragments now in effect (a region whose job
//                         failed this week keeps last week's)
//     upload.txt          every key above that must be (re)uploaded
//     delete.txt          tile and index keys no longer published
//
// index.json, for the cells of one group that some region built:
//   {"v":1,"cells":{"<h3 id>":[ebmSize,"ebmHash",poiSize,"poiHash"], ...}}
// ebmSize 0: the cell was built and is empty (nothing to draw) — the apps
// must not ask Overpass for it. poiSize 0: no POIs (the apps synthesise the
// empty .poi, as they do for an Overpass answer with none). A cell missing
// from its group's index, or a group with no index.json (HTTP 404), was not
// built: the apps fall back to Overpass. The hash goes on the request as
// ?v=<hash>, so a tile URL changes whenever its bytes do and the CDN can
// cache tiles for long; only index.json needs a short max-age.
import fs from "node:fs";
import path from "node:path";
import { LAYOUT_VERSION, tileKey } from "./build_region.mjs";

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

// Every *.json under dir (recursively) that looks like a fragment.
function readFragments(dir) {
  const out = new Map();
  if (!dir || !fs.existsSync(dir)) return out;
  const walk = (d) => {
    for (const e of fs.readdirSync(d, { withFileTypes: true })) {
      const p = path.join(d, e.name);
      if (e.isDirectory()) walk(p);
      else if (e.name.endsWith(".json")) {
        let f;
        try { f = JSON.parse(fs.readFileSync(p, "utf8")); } catch { continue; }
        if (f && f.region && f.cells && f.version === LAYOUT_VERSION) out.set(f.region, f);
      }
    }
  };
  walk(dir);
  return out;
}

// cell -> record, earlier regions (in `order`) winning a tie.
function cellsOf(frags, order) {
  const rank = (r) => { const k = order.indexOf(r); return k < 0 ? order.length : k; };
  const regs = [...frags.keys()].sort((x, y) => rank(x) - rank(y) || (x < y ? -1 : x > y ? 1 : 0));
  const cells = new Map();
  let dupes = 0;
  for (const r of regs) {
    for (const [id, rec] of Object.entries(frags.get(r).cells)) {
      if (cells.has(id)) { dupes++; continue; }
      cells.set(id, rec);
    }
  }
  return { cells, dupes };
}

function groups(cells) {
  const g = new Map();
  for (const id of [...cells.keys()].sort()) {
    const k = id.slice(0, 6);
    let m = g.get(k); if (!m) g.set(k, (m = {})); m[id] = cells.get(id);
  }
  return g;
}

const indexJson = (m) => JSON.stringify({ v: 1, cells: m });
const liveKeys = (cells) => {
  const s = new Set();
  for (const [id, r] of cells) { if (r[0]) s.add(tileKey(id, ".ebm")); if (r[2]) s.add(tileKey(id, ".poi")); }
  return s;
};

export function merge({ newFrags, oldFrags, order = [] }) {
  // A region with no new fragment keeps its old one (its job failed or was
  // skipped), so a bad week never unpublishes a country.
  const now = new Map(oldFrags);
  for (const [r, f] of newFrags) now.set(r, f);
  const oldC = cellsOf(oldFrags, order), newC = cellsOf(now, order);
  const oldG = groups(oldC.cells), newG = groups(newC.cells);
  const indexes = new Map(), del = [];
  for (const [g, m] of newG) {
    const j = indexJson(m);
    const o = oldG.get(g);
    if (!o || indexJson(o) !== j) indexes.set(g, j);
  }
  for (const g of oldG.keys()) if (!newG.has(g)) del.push(`${LAYOUT_VERSION}/${g}/index.json`);
  const live = liveKeys(newC.cells);
  for (const k of liveKeys(oldC.cells)) if (!live.has(k)) del.push(k);
  return { now, indexes, del: del.sort(), cells: newC.cells.size, groups: newG.size, dupes: newC.dupes };
}

async function main() {
  const a = args();
  if (!a.new || !a.out) {
    console.error("usage: merge_index.mjs --new <dir> [--old <dir>] --out <dir> [--regions regions.json]");
    process.exit(2);
  }
  const order = a.regions ? JSON.parse(fs.readFileSync(a.regions, "utf8")).regions.map((r) => r.id) : [];
  const newFrags = readFragments(a.new), oldFrags = readFragments(a.old);
  const r = merge({ newFrags, oldFrags, order });
  const out = path.resolve(a.out);
  const upload = [];
  const put = (key, body) => {
    fs.mkdirSync(path.join(out, path.dirname(key)), { recursive: true });
    fs.writeFileSync(path.join(out, key), body);
    upload.push(key);
  };
  for (const [g, j] of r.indexes) put(`${LAYOUT_VERSION}/${g}/index.json`, j);
  for (const [reg, f] of r.now) if (newFrags.has(reg)) put(`${LAYOUT_VERSION}/regions/${reg.replace(/\//g, "_")}.json`, JSON.stringify(f));
  const meta = {
    version: LAYOUT_VERSION, merged: new Date().toISOString(), cells: r.cells, groups: r.groups,
    regions: Object.fromEntries([...r.now].sort(([x], [y]) => (x < y ? -1 : 1)).map(([reg, f]) =>
      [reg, { osm: f.osm, built: f.built, cells: Object.keys(f.cells).length, fresh: newFrags.has(reg) }])),
  };
  put(`${LAYOUT_VERSION}/meta.json`, JSON.stringify(meta, null, 1));
  fs.writeFileSync(path.join(out, "upload.txt"), upload.join("\n") + "\n");
  fs.writeFileSync(path.join(out, "delete.txt"), r.del.join("\n") + (r.del.length ? "\n" : ""));
  console.error(`merge: ${r.now.size} regions (${newFrags.size} fresh), ${r.cells} cells in ${r.groups} groups, ` +
    `${r.indexes.size} index.json changed, ${r.del.length} deletions, ${r.dupes} duplicate cells resolved by region order`);
}

if (process.argv[1] && import.meta.url === `file://${path.resolve(process.argv[1])}`) await main();
