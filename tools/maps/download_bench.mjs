// Where does a map download's time go? Replays the phone apps' network pipeline
// (MapsView.swift / MapsSheet.kt) for a set of H3 res-6 hexes and times every
// request: Overpass wait (time to first byte), transfer, bytes, JSON parse and
// the encode (docs/mapgen.js buildEbm/buildPoi as a stand-in for the app's
// MapBuilder — same algorithm, timed on this machine).
//
//   node tools/maps/download_bench.mjs <lat> <lon> <hexes> <mode> [label]
//     mode: app    — what the apps did before this change: sequential map
//                    batches 1 s apart, 4 sequential elevation calls per hex,
//                    sequential POI groups, overpass-api.de first
//           fast   — the quick wins as the apps now do them: map batches 2
//                    at a time, each starting on a different mirror
//                    (overpass-api.de, private.coffee, mail.ru), coastline
//                    and POIs alongside, elevation 4 hexes at a time with
//                    each hex's 4 calls in parallel
//   H3JS_DIR=<dir with node_modules/h3-js> is required (hex selection).
//
// Be polite: every run is real load on public Overpass servers.
import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";
import { buildEbm, buildPoi, collectPois } from "../../docs/mapgen.js";

const [lat, lon, nHex, mode = "app", label = ""] = process.argv.slice(2);
const h3 = createRequire(path.join(process.env.H3JS_DIR || ".", "x.js"))("h3-js");
const UA = "OpenTrailPaper/bench (download timing; +https://opentrailpaper.com)";

// The apps' queries, read from the iOS source so this cannot drift.
const swift = fs.readFileSync(new URL("../../companion-ios/Sources/MapBuilder.swift", import.meta.url), "utf8");
const grab = (name) => swift.match(new RegExp(`static let ${name} = """\\n([\\s\\S]*?)\\n\\s*"""`))[1]
  .split("\n").map((l) => l.replace(/^ {4}/, "")).join("\n");
const MAP_Q = grab("query"), POI_Q = grab("poiQuery");

const MIRRORS = {
  app: ["https://overpass-api.de/api/interpreter",
        "https://maps.mail.ru/osm/tools/overpass/api/interpreter"],
  fast: ["https://overpass-api.de/api/interpreter",
         "https://overpass.private.coffee/api/interpreter",
         "https://maps.mail.ru/osm/tools/overpass/api/interpreter"],
};

// ---- hex selection: the nHex cells nearest the centre
function cellBbox(c) {
  let s = 90, w = 180, n = -90, e = -180;
  for (const [la, lo] of h3.cellToBoundary(c)) { s = Math.min(s, la); n = Math.max(n, la); w = Math.min(w, lo); e = Math.max(e, lo); }
  return { id: c, s, w, n, e };
}
const centre = h3.latLngToCell(+lat, +lon, 6);
const tiles = [];
for (let k = 0; tiles.length < +nHex; k++) {
  for (const c of h3.gridRingUnsafe ? h3.gridRingUnsafe(centre, k) : h3.gridDisk(centre, k)) {
    if (!tiles.some((t) => t.id === c) && tiles.length < +nHex) tiles.push(cellBbox(c));
  }
}
const union = (ts, pad = 0.003) => {
  let s = 90, w = 180, n = -90, e = -180;
  for (const t of ts) { s = Math.min(s, t.s); w = Math.min(w, t.w); n = Math.max(n, t.n); e = Math.max(e, t.e); }
  return { s: s - pad, w: w - pad, n: n + pad, e: e + pad };
};
const groupBy = (ts, deg) => Object.values(ts.reduce((m, t) => {
  const k = `${Math.floor((t.s + t.n) / 2 / deg)}_${Math.floor((t.w + t.e) / 2 / deg)}`;
  (m[k] ||= []).push(t); return m;
}, {}));
const bb = (u) => `${u.s},${u.w},${u.n},${u.e}`;

// ---- timed requests
const log = [];
const t0 = performance.now();
const now = () => (performance.now() - t0) / 1000;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function timed(kind, url, init, attempt = 0) {
  const start = now();
  let status = 0, bytes = 0, ttfb = 0, body = null;
  try {
    const ctl = new AbortController();
    const to = setTimeout(() => ctl.abort(), 90_000);
    const r = await fetch(url, { ...init, signal: ctl.signal, headers: { ...(init?.headers || {}), "User-Agent": UA } });
    ttfb = now() - start;
    status = r.status;
    const buf = Buffer.from(await r.arrayBuffer());
    clearTimeout(to);
    bytes = buf.length;
    body = status === 200 ? buf : null;
  } catch (e) { status = e.name === "AbortError" ? "timeout" : "error"; }
  const end = now();
  log.push({ kind, host: new URL(url).host, start, ttfb, end, status, bytes, attempt });
  return body;
}

async function overpass(kind, q, mirrors, rotateFrom = 0) {
  const body = "data=" + encodeURIComponent(q);
  for (let a = 0; a < mirrors.length * 2; a++) {
    const url = mirrors[(rotateFrom + a) % mirrors.length];
    const b = await timed(kind, url, { method: "POST", body,
      headers: { "Content-Type": "application/x-www-form-urlencoded" } }, a);
    if (b) return b;
    await sleep(800);
  }
  failedJobs.push(kind);   // the app would report these hexes as failed
  return null;
}

const failedJobs = [];
let parseS = 0, encodeS = 0, ebmBytes = 0, poiBytes = 0, poiCount = 0;
function parse(buf) { if (!buf) return null; const a = performance.now(); const j = JSON.parse(buf.toString("utf8")); parseS += (performance.now() - a) / 1000; return j; }

let gateNext = 0;
async function gate() {
  const at = Math.max(Date.now(), gateNext); gateNext = at + 1100;
  if (at > Date.now()) await sleep(at - Date.now());
}
async function elevation(t, parallel) {
  const N = 20, lats = [], lons = [];
  for (let i = 0; i < N; i++) for (let j = 0; j < N; j++) {
    lats.push((t.s + (t.n - t.s) * i / (N - 1)).toFixed(5));
    lons.push((t.w + (t.e - t.w) * j / (N - 1)).toFixed(5));
  }
  const calls = [];
  for (let i = 0; i < lats.length; i += 100) {
    calls.push(`https://api.open-meteo.com/v1/elevation?latitude=${lats.slice(i, i + 100).join(",")}&longitude=${lons.slice(i, i + 100).join(",")}`);
  }
  if (parallel) {
    // As the apps now do: calls spaced 1.1 s apart app-wide (Open-Meteo weighs
    // a 100-point call as 10 against 600/min), one retry after 20 s on 429.
    await Promise.all(calls.map(async (u) => {
      await gate();
      if (!(await timed("elevation", u))) { await sleep(20000); await gate(); await timed("elevation", u); }
    }));
  } else for (const u of calls) await timed("elevation", u);
}

async function pool(items, n, fn) {
  let i = 0;
  await Promise.all(Array.from({ length: Math.min(n, items.length) }, async () => {
    while (i < items.length) { const k = i++; await fn(items[k], k); }
  }));
}

function encodeBatch(json, batch) {
  if (!json) return;
  const a = performance.now();
  for (const t of batch) ebmBytes += buildEbm(json, t).length;
  encodeS += (performance.now() - a) / 1000;
}
function encodePois(json, group) {
  if (!json) return;
  const a = performance.now();
  const pois = collectPois(json);
  for (const t of group) {
    const f = buildPoi(json, { ...t, cell: t.id, contains: (la, lo) => h3.latLngToCell(la, lo, 6) === t.id });
    poiBytes += f.length; poiCount += f[6] | (f[7] << 8);
  }
  void pois;
  encodeS += (performance.now() - a) / 1000;
}

const all = union(tiles, 0);
const fast = mode === "fast";
const mirrors = MIRRORS[fast ? "fast" : "app"];
const batches = groupBy(tiles, 0.08);
const poiGroups = groupBy(tiles, 0.25);
const coastQ = `[out:json][timeout:60];way["natural"="coastline"](${bb({ s: all.s - 0.35, w: all.w - 0.35, n: all.n + 0.35, e: all.e + 0.35 })});(._;>;);out body;`;

const stage = {};
const mark = async (name, f) => { const a = now(); await f(); stage[name] = now() - a; };

if (!fast) {
  await mark("coastline", () => overpass("coastline", coastQ, mirrors));
  await mark("map+elevation", async () => {
    for (const [i, b] of batches.entries()) {
      if (i) await sleep(1000);
      const json = parse(await overpass("map", MAP_Q.replaceAll("{B}", bb(union(b))), mirrors));
      encodeBatch(json, b);
      for (const t of b) await elevation(t, false);
    }
  });
  await mark("pois", async () => {
    for (const [i, g] of poiGroups.entries()) {
      if (i) await sleep(1000);
      encodePois(parse(await overpass("poi", POI_Q.replaceAll("{B}", bb(union(g))), mirrors)), g);
    }
  });
} else {
  // Everything starts at once: coastline, POIs and two map batches on
  // different mirrors; elevation for 4 hexes at a time as soon as each batch
  // is built. Overpass gets at most 2 concurrent requests from us (its
  // per-IP limit is 4 slots), each mirror at most one map batch at a time.
  await mark("all", () => Promise.all([
    overpass("coastline", coastQ, mirrors, 1),
    pool(poiGroups, 1, async (g) => encodePois(parse(await overpass("poi", POI_Q.replaceAll("{B}", bb(union(g))), mirrors, 1)), g)),
    pool(batches, 2, async (b, k) => {
      const json = parse(await overpass("map", MAP_Q.replaceAll("{B}", bb(union(b))), mirrors, k));
      encodeBatch(json, b);
      await pool(b, 2, (t) => elevation(t, true));
    }),
  ]));
}

const total = now();
const by = (k) => log.filter((r) => r.kind === k);
const sum = (rs, f) => rs.reduce((a, r) => a + f(r), 0);
const summary = {
  label, mode, failedJobs, hexes: tiles.length, mapBatches: batches.length, poiGroups: poiGroups.length,
  totalS: +total.toFixed(1), stages: Object.fromEntries(Object.entries(stage).map(([k, v]) => [k, +v.toFixed(1)])),
  parseS: +parseS.toFixed(2), encodeS: +encodeS.toFixed(2), ebmKB: Math.round(ebmBytes / 1024),
  poiBytes, poiCount,
};
for (const k of ["coastline", "map", "poi", "elevation"]) {
  const rs = by(k);
  summary[k] = {
    requests: rs.length, failed: rs.filter((r) => r.status !== 200).length,
    waitS: +sum(rs, (r) => r.ttfb).toFixed(1), transferS: +sum(rs, (r) => r.end - r.start - r.ttfb).toFixed(1),
    MB: +(sum(rs, (r) => r.bytes) / 1048576).toFixed(2),
    hosts: [...new Set(rs.filter((r) => r.status === 200).map((r) => r.host))],
    errors: rs.filter((r) => r.status !== 200).map((r) => `${r.host}:${r.status}`),
  };
}
console.log(JSON.stringify(summary));
if (process.env.BENCH_LOG) fs.writeFileSync(process.env.BENCH_LOG, JSON.stringify(log, null, 1));
