// Load an OSM extract into compact typed arrays, ready for per-cell
// Overpass emulation (overpass.mjs).
//
//   osmium tags-filter <pbf> -e filters.txt        (what the queries can match,
//                                                   + referenced members/nodes)
//   osmium add-locations-to-ways -f opl            (way node refs carry x/y)
//
// and stream the OPL text. Only the tags the builders read are kept, interned
// (most ways share a handful of tag sets). Coordinates are kept as the OSM
// 1e-7 degree integers, which is lossless: Overpass prints 7 decimals, and
// int/1e7 is the same double as parsing that decimal string.
import { spawn } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import readline from "node:readline";

// Tags any builder reads (mapgen classify/isPark/wayFlags/poiOf, the apps'
// water/park extractors) or the Overpass predicates test.
const KEEP_KEYS = new Set([
  "highway", "footway", "bicycle", "cycleway", "cycleway:both", "cycleway:left", "cycleway:right",
  "natural", "leisure", "landuse", "amenity", "man_made", "drinking_water", "shop",
  "access", "fee", "seasonal", "route", "network",
]);
const keepKey = (k) => KEEP_KEYS.has(k) || k.startsWith("service:bicycle:");

// ---- the Overpass predicates (companion-ios MapBuilder.query / poiQuery) ----
const HW_RE = /^(motorway|trunk|primary|secondary|tertiary|residential|unclassified|living_street|pedestrian|cycleway|footway|path|track|steps)/;
const LANDUSE_RE = /^(grass|forest|meadow|recreation_ground|cemetery|village_green)$/;
const NATURAL_RE = /^(wood|scrub|grassland|heath)$/;
const POI_AMENITY_NODE_RE = /^(drinking_water|toilets|bicycle_repair_station)$/;
const POI_AMENITY_WAY_RE = /^(toilets|bicycle_repair_station)$/;

export function mapWayMatch(t) {
  return (t.highway != null && HW_RE.test(t.highway)) ||
    t.natural === "water" || t.natural === "coastline" || t.leisure === "park" ||
    (t.landuse != null && LANDUSE_RE.test(t.landuse)) ||
    (t.natural != null && NATURAL_RE.test(t.natural));
}
export function poiNodeMatch(t) {
  return (t.amenity != null && POI_AMENITY_NODE_RE.test(t.amenity)) ||
    (t.man_made === "water_tap" && t.drinking_water === "yes") ||
    (t.amenity === "fountain" && t.drinking_water === "yes") ||
    t.shop === "bicycle";
}
export function poiWayMatch(t) {
  return (t.amenity != null && POI_AMENITY_WAY_RE.test(t.amenity)) || t.shop === "bicycle";
}

export const K_MAP = 1, K_COAST = 2, K_POI = 4;

// Growable typed array.
class Grow {
  constructor(Type, cap = 1 << 16) { this.Type = Type; this.a = new Type(cap); this.n = 0; }
  push(v) {
    if (this.n === this.a.length) { const b = new this.Type(this.a.length * 2); b.set(this.a); this.a = b; }
    this.a[this.n++] = v;
  }
  done() { return this.a.subarray(0, this.n); }
}

// "x-122.4194155" body (without the x) -> 1e-7 integer, or null.
function fixed7(s) {
  if (!s) return null;
  let neg = false, i = 0;
  if (s[0] === "-") { neg = true; i = 1; }
  const dot = s.indexOf(".", i);
  const whole = dot < 0 ? s.slice(i) : s.slice(i, dot);
  let frac = dot < 0 ? "" : s.slice(dot + 1);
  if (frac.length > 7) return null;
  frac = frac.padEnd(7, "0");
  const v = Number(whole) * 1e7 + Number(frac);
  if (!Number.isFinite(v)) return null;
  return neg ? -v : v;
}

const unesc = (s) => (s.indexOf("%") < 0 ? s : s.replace(/%([0-9a-fA-F]+)%/g, (_, h) => String.fromCodePoint(parseInt(h, 16))));

function parseTags(field) {
  // field without the leading "T"
  const t = {};
  if (!field) return t;
  for (const kv of field.split(",")) {
    const eq = kv.indexOf("=");
    if (eq < 0) continue;
    const k = unesc(kv.slice(0, eq));
    if (!keepKey(k)) continue;
    t[k] = unesc(kv.slice(eq + 1));
  }
  return t;
}

function run(cmd, args) {
  return new Promise((resolve, reject) => {
    const p = spawn(cmd, args, { stdio: ["ignore", "inherit", "inherit"] });
    p.on("error", reject);
    p.on("exit", (code) => (code === 0 ? resolve() : reject(new Error(`${cmd} ${args.join(" ")} exited ${code}`))));
  });
}

const MISSING = -2147483648;   // Int32 sentinel: node location not in the extract

export async function loadOsm(pbf, { tmpDir, filters = new URL("./filters.txt", import.meta.url).pathname, log = () => {} } = {}) {
  fs.mkdirSync(tmpDir, { recursive: true });
  const filtered = path.join(tmpDir, "filtered.osm.pbf");
  let t0 = Date.now();
  await run("osmium", ["tags-filter", pbf, "-e", filters, "-O", "-o", filtered, "--no-progress"]);
  log(`osmium tags-filter: ${((Date.now() - t0) / 1000).toFixed(1)} s, ${(fs.statSync(filtered).size / 1048576).toFixed(1)} MB`);
  t0 = Date.now();

  const tagList = [];          // interned tag objects
  const tagIdx = new Map();    // canonical key -> index
  const intern = (t) => {
    const key = Object.keys(t).sort().map((k) => k + "\u0001" + t[k]).join("\u0002");
    let i = tagIdx.get(key);
    if (i === undefined) { i = tagList.length; tagList.push(Object.freeze(t)); tagIdx.set(key, i); }
    return i;
  };

  // Ways.
  const wId = new Grow(Float64Array), wStart = new Grow(Uint32Array), wLen = new Grow(Uint32Array);
  const wTag = new Grow(Uint32Array), wKind = new Grow(Uint8Array);
  const rId = new Grow(Float64Array), rLat = new Grow(Int32Array), rLon = new Grow(Int32Array);
  const wayIndex = new Map();  // way id -> index (for relation members)
  // POI nodes.
  const pois = [];             // { kind: "n"|"w"|"r", id, tags, lat, lon } (lat/lon as doubles)
  // Bike-route levels and POI relations.
  const routeLevel = new Map();   // way id -> 1..3
  const poiRels = [];

  const p = spawn("osmium", ["add-locations-to-ways", filtered, "--ignore-missing-nodes",
    "-f", "opl,add_metadata=false", "-o", "-", "--no-progress"], { stdio: ["ignore", "pipe", "inherit"] });
  const exited = new Promise((resolve, reject) => {
    p.on("error", reject);
    p.on("exit", (code) => (code === 0 ? resolve() : reject(new Error(`add-locations-to-ways exited ${code}`))));
  });
  const rl = readline.createInterface({ input: p.stdout, crlfDelay: Infinity });
  let nLines = 0, lastId = -1, sorted = true;
  for await (const line of rl) {
    nLines++;
    const c = line.charCodeAt(0);
    if (c === 110 /* n */) {
      // n<id> T<tags> x<lon> y<lat>
      const f = line.split(" ");
      let tags = null, x = null, y = null;
      for (let i = 1; i < f.length; i++) {
        const h = f[i][0];
        if (h === "T") tags = f[i].slice(1);
        else if (h === "x") x = fixed7(f[i].slice(1));
        else if (h === "y") y = fixed7(f[i].slice(1));
      }
      if (!tags || x == null || y == null) continue;
      const t = parseTags(tags);
      if (!poiNodeMatch(t)) continue;
      pois.push({ kind: "n", id: Number(f[0].slice(1)), tags: t, lat: y / 1e7, lon: x / 1e7 });
    } else if (c === 119 /* w */) {
      const f = line.split(" ");
      const id = Number(f[0].slice(1));
      if (id <= lastId) sorted = false;
      lastId = id;
      let tags = "", refs = "";
      for (let i = 1; i < f.length; i++) {
        const h = f[i][0];
        if (h === "T") tags = f[i].slice(1);
        else if (h === "N") refs = f[i].slice(1);
      }
      const t = parseTags(tags);
      let kind = 0;
      if (mapWayMatch(t)) kind |= K_MAP;
      if (t.natural === "coastline") kind |= K_COAST;
      if (poiWayMatch(t)) kind |= K_POI;
      const idx = wId.n;
      wayIndex.set(id, idx);
      wId.push(id); wStart.push(rId.n); wTag.push(intern(t)); wKind.push(kind);
      let cnt = 0;
      if (refs) {
        for (const r of refs.split(",")) {
          // n<id>x<lon>y<lat>   (x/y empty when the location is missing)
          const xi = r.indexOf("x"), yi = r.indexOf("y");
          const nid = Number(r.slice(1, xi < 0 ? undefined : xi));
          const lon = xi < 0 ? null : fixed7(r.slice(xi + 1, yi < 0 ? undefined : yi));
          const lat = yi < 0 ? null : fixed7(r.slice(yi + 1));
          rId.push(nid);
          rLat.push(lat == null ? MISSING : lat);
          rLon.push(lon == null ? MISSING : lon);
          cnt++;
        }
      }
      wLen.push(cnt);
    } else if (c === 114 /* r */) {
      const f = line.split(" ");
      const id = Number(f[0].slice(1));
      let tags = "", mem = "";
      for (let i = 1; i < f.length; i++) {
        const h = f[i][0];
        if (h === "T") tags = f[i].slice(1);
        else if (h === "M") mem = f[i].slice(1);
      }
      const t = parseTags(tags);
      const members = mem ? mem.split(",").map((m) => { const at = m.indexOf("@"); return [m[0], Number(m.slice(1, at < 0 ? undefined : at))]; }) : [];
      if (t.route === "bicycle") {
        const net = t.network || "";
        const lvl = net === "icn" || net === "ncn" ? 3 : net === "rcn" ? 2 : 1;
        for (const [ty, mid] of members) {
          if (ty !== "w") continue;
          if ((routeLevel.get(mid) || 0) < lvl) routeLevel.set(mid, lvl);
        }
      }
      if (t.shop === "bicycle") poiRels.push({ id, tags: t, members });
    }
  }
  await exited;
  const ways = {
    id: wId.done(), start: wStart.done(), len: wLen.done(), tag: wTag.done(), kind: wKind.done(),
    level: new Uint8Array(wId.n),
  };
  const refs = { id: rId.done(), lat: rLat.done(), lon: rLon.done() };
  for (const [wid, lvl] of routeLevel) {
    const i = wayIndex.get(wid);
    if (i !== undefined) ways.level[i] = lvl;
  }

  // Way POIs and relation POIs sit at the Overpass `out center` point: the
  // centre of the geometry's bounding box, printed with 7 decimals.
  const bboxOf = (idxs) => {
    let s = Infinity, w = Infinity, n = -Infinity, e = -Infinity;
    for (const i of idxs) {
      for (let k = ways.start[i]; k < ways.start[i] + ways.len[i]; k++) {
        if (refs.lat[k] === MISSING) continue;
        const la = refs.lat[k] / 1e7, lo = refs.lon[k] / 1e7;
        if (la < s) s = la; if (la > n) n = la; if (lo < w) w = lo; if (lo > e) e = lo;
      }
    }
    return s === Infinity ? null : { lat: Number(((s + n) / 2).toFixed(7)), lon: Number(((w + e) / 2).toFixed(7)) };
  };
  for (let i = 0; i < ways.id.length; i++) {
    if (!(ways.kind[i] & K_POI)) continue;
    const c = bboxOf([i]);
    if (c) pois.push({ kind: "w", id: ways.id[i], tags: tagList[ways.tag[i]], lat: c.lat, lon: c.lon });
  }
  for (const r of poiRels) {
    const idxs = r.members.filter(([ty]) => ty === "w").map(([, mid]) => wayIndex.get(mid)).filter((i) => i !== undefined);
    const c = bboxOf(idxs);
    if (c) pois.push({ kind: "r", id: r.id, tags: r.tags, lat: c.lat, lon: c.lon });
  }
  log(`OPL: ${nLines} lines, ${ways.id.length} ways, ${refs.id.length} refs, ${tagList.length} tag sets, ` +
      `${routeLevel.size} route ways, ${pois.length} POIs, ${((Date.now() - t0) / 1000).toFixed(1)} s` +
      (sorted ? "" : " (ways NOT sorted by id — will sort per query)"));
  return { ways, refs, tags: tagList, pois, sorted };
}

export { MISSING };
