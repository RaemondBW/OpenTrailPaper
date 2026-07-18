// In-browser offline-map builder for OpenCycleInk.
//
// A straight port of tools/maps/build_map.py (and companion-ios MapBuilder.swift)
// so the output is byte-compatible with the .ebm whole-region maps the firmware
// reads from /maps/*.ebm. Fetch OSM ways from Overpass for a bounding box,
// classify + simplify them, clip into the fixed lat/lon tile grid, and encode
// the binary — all locally in the browser, nothing uploaded anywhere.
//
// Format (little-endian):
//   'EBM2', f64 lat0, f64 lon0, f64 tileDeg, i32 nx, i32 ny,
//   index[nx*ny] of (u32 offset, u32 length)  (0,0 = empty tile),
//   tiles: u16 polylineCount, per polyline { u8 class, u16 pointCount,
//          i16 x,y per point (metres E/N of the tile SW corner) }.
//   class: 0 arterial, 1 secondary, 2 minor, 3 path.
//   water: 'WTR2', u16 polygonCount, per polygon { u16 pointCount,
//          i16 x,y per point (metres E/N of the grid SW origin lat0,lon0) }.

export const TILE_DEG = 0.02;
export const SIMPLIFY_M = 3.0;

// Public Overpass instances, tried in order with a retry pass (mirrors the iOS
// app). The busy main instance 504s under load, so rotating usually succeeds.
export const OVERPASS_ENDPOINTS = [
  "https://overpass-api.de/api/interpreter",
  "https://maps.mail.ru/osm/tools/overpass/api/interpreter",
];

const QUERY = `[out:json][timeout:90];
(
  way["highway"~"^(motorway|trunk|primary|secondary|tertiary|residential|unclassified|living_street|pedestrian|cycleway|footway|path|track|steps)"](%S,%W,%N,%E);
  way["natural"="water"](%S,%W,%N,%E);
  way["natural"="coastline"](%S,%W,%N,%E);
);
out body;
>;
out skel qt;`;

// Road tiers (device render classes).
const ARTERIAL = new Set(["motorway", "trunk", "primary"]);
const SECONDARY = new Set(["secondary", "tertiary"]);
const MINOR = new Set(["residential", "unclassified", "living_street", "pedestrian"]);
const PATH = new Set(["cycleway", "footway", "path", "track", "steps"]);

// OSM tags -> road tier (0 arterial, 1 secondary, 2 minor, 3 path), or null to
// drop. Rail is dropped; natural=water is handled separately (WTR2). Must agree
// with build_map.py / MapBuilder.swift.
export function classify(tags) {
  if (tags == null) return null;
  const hw = tags.highway || "";
  // Sidewalks and crossings duplicate every street — noise on a bike map.
  if (hw === "footway" && (tags.footway === "sidewalk" || tags.footway === "crossing")) return null;
  const base = hw.split("_link")[0];
  if (ARTERIAL.has(base)) return 0;
  if (SECONDARY.has(base)) return 1;
  if (MINOR.has(base)) return 2;
  if (PATH.has(base)) return 3;
  return null;
}

// Ramer–Douglas–Peucker on projected metre coords. points: [[x,y], …].
export function rdp(points, eps) {
  const nPts = points.length;
  if (nPts < 3) return points.slice();
  const keep = new Array(nPts).fill(false);
  keep[0] = keep[nPts - 1] = true;
  const stack = [[0, nPts - 1]];
  while (stack.length) {
    const [a, b] = stack.pop();
    const [ax, ay] = points[a];
    const [bx, by] = points[b];
    const dx = bx - ax, dy = by - ay;
    const norm = Math.hypot(dx, dy) || 1e-9;
    let worst = 0.0, wi = -1;
    for (let i = a + 1; i < b; i++) {
      const [px, py] = points[i];
      const d = Math.abs(dx * (ay - py) - dy * (ax - px)) / norm;
      if (d > worst) { worst = d; wi = i; }
    }
    if (worst > eps && wi >= 0) {
      keep[wi] = true;
      stack.push([a, wi]);
      stack.push([wi, b]);
    }
  }
  const out = [];
  for (let i = 0; i < nPts; i++) if (keep[i]) out.push(points[i]);
  return out;
}

// Radial decimation for closed rings (water): keep the first point, then each
// point only if it's > eps metres from the last kept point. Unlike RDP this
// survives closed rings (the implicit closing point drops at distance 0).
export function decimate(points, eps) {
  if (points.length === 0) return points.slice();
  const kept = [points[0]];
  let lx = points[0][0], ly = points[0][1];
  for (let i = 1; i < points.length; i++) {
    const [x, y] = points[i];
    if (Math.hypot(x - lx, y - ly) > eps) {
      kept.push([x, y]);
      lx = x; ly = y;
    }
  }
  return kept;
}

// --- coastline -> sea polygons ---------------------------------------------
// OSM splits coastlines into many ways that share endpoints; each way keeps
// LAND on its LEFT and SEA on its RIGHT. Assemble ways into maximal chains
// (preserving that direction), clip each chain to a tile rectangle, and close
// each boundary-to-boundary sub-chain along the rectangle edge that encloses
// the SEA side — yielding filled WTR2 sea polygons. Must agree byte-for-byte
// with build_map.py.

// Positive modulo (matches Python's % for the float perimeter math).
function pmod(a, b) { return ((a % b) + b) % b; }

// Join coastline ways (node-id lists) into maximal chains of [lat,lon]. Ways
// are joined end-to-end; a way is reversed only to make endpoints meet.
export function assembleCoastline(coastWays, nodes) {
  const chains = coastWays.map((nids) => nids.slice());
  let changed = true;
  while (changed) {
    changed = false;
    for (let i = 0; i < chains.length && !changed; i++) {
      if (chains[i] == null) continue;
      for (let j = 0; j < chains.length && !changed; j++) {
        if (j === i || chains[j] == null) continue;
        const a = chains[i];
        const b = chains[j];
        if (a[a.length - 1] === b[0]) {
          chains[i] = a.concat(b.slice(1));
          chains[j] = null; changed = true;
        } else if (a[a.length - 1] === b[b.length - 1]) {
          chains[i] = a.concat(b.slice(0, -1).reverse());
          chains[j] = null; changed = true;
        } else if (a[0] === b[b.length - 1]) {
          chains[i] = b.concat(a.slice(1));
          chains[j] = null; changed = true;
        } else if (a[0] === b[0]) {
          chains[i] = b.slice().reverse().concat(a.slice(1));
          chains[j] = null; changed = true;
        }
      }
    }
  }
  const out = [];
  for (const c of chains) {
    if (c == null) continue;
    const pts = [];
    for (const id of c) { const p = nodes.get(id); if (p) pts.push(p); }
    if (pts.length >= 2) out.push(pts);
  }
  return out;
}

// Clip segment a->b (points [lat,lon]) to the rectangle. Returns [t0,t1] with
// 0<=t0<=t1<=1, or null if the segment is entirely outside.
function liangBarsky(a, b, s, w, n, e) {
  const ax = a[1], ay = a[0]; // x=lon, y=lat
  const bx = b[1], by = b[0];
  const dx = bx - ax, dy = by - ay;
  const p = [-dx, dx, -dy, dy];
  const q = [ax - w, e - ax, ay - s, n - ay];
  let t0 = 0.0, t1 = 1.0;
  for (let i = 0; i < 4; i++) {
    if (p[i] === 0.0) {
      if (q[i] < 0.0) return null;
    } else {
      const t = q[i] / p[i];
      if (p[i] < 0.0) {
        if (t > t1) return null;
        if (t > t0) t0 = t;
      } else {
        if (t < t0) return null;
        if (t < t1) t1 = t;
      }
    }
  }
  return [t0, t1];
}

function lerp(a, b, t) {
  return [a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])];
}

function ptEq(a, b) { return a[0] === b[0] && a[1] === b[1]; }

// Split a chain into rectangle-clipped sub-chains. Each result is
// [points, startOnBoundary, endOnBoundary].
export function clipChain(chain, s, w, n, e) {
  const subs = [];
  let cur = null;
  let startB = false;
  for (let k = 0; k < chain.length - 1; k++) {
    const a = chain[k], b = chain[k + 1];
    const lb = liangBarsky(a, b, s, w, n, e);
    if (lb == null) {
      if (cur != null) { subs.push([cur, startB, false]); cur = null; }
      continue;
    }
    const [t0, t1] = lb;
    // Use the exact chain endpoint when unclipped, so a shared point matches
    // bit-for-bit across adjacent segments (avoids a degenerate zero-length
    // segment from float drift in lerp(a,b,1)).
    const p0 = t0 === 0.0 ? a : lerp(a, b, t0);
    const p1 = t1 === 1.0 ? b : lerp(a, b, t1);
    if (cur == null) {
      cur = [p0];
      startB = t0 > 0.0;
    } else if (!ptEq(cur[cur.length - 1], p0)) {
      cur.push(p0);
    }
    if (!ptEq(cur[cur.length - 1], p1)) cur.push(p1);
    if (t1 < 1.0) { subs.push([cur, startB, true]); cur = null; }
  }
  if (cur != null) subs.push([cur, startB, false]);
  return subs;
}

// Position of a boundary point along the perimeter, CCW from the SW corner.
function perimPos(pt, s, w, n, e) {
  const lat = pt[0], lon = pt[1];
  const ww = e - w, hh = n - s;
  const db = Math.abs(lat - s), dr = Math.abs(lon - e);
  const dt = Math.abs(lat - n), dl = Math.abs(lon - w);
  const mn = Math.min(db, dr, dt, dl);
  if (mn === db) return lon - w;
  if (mn === dr) return ww + (lat - s);
  if (mn === dt) return ww + hh + (e - lon);
  return ww + hh + ww + (n - lat);
}

// Corner points passed walking the perimeter from fromPos to toPos, CCW
// (increasing) or CW (decreasing).
function closing(fromPos, toPos, s, w, n, e, ccw) {
  const ww = e - w, hh = n - s;
  const total = 2 * ww + 2 * hh;
  const corners = [
    [s, w, 0.0],
    [s, e, ww],
    [n, e, ww + hh],
    [n, w, ww + hh + ww],
  ];
  const res = [];
  if (ccw) {
    const d = pmod(toPos - fromPos, total);
    for (const [clat, clon, cpos] of corners) {
      const cd = pmod(cpos - fromPos, total);
      if (cd > 0.0 && cd < d) res.push([cd, [clat, clon]]);
    }
  } else {
    const d = pmod(fromPos - toPos, total);
    for (const [clat, clon, cpos] of corners) {
      const cd = pmod(fromPos - cpos, total);
      if (cd > 0.0 && cd < d) res.push([cd, [clat, clon]]);
    }
  }
  res.sort((x, y) => x[0] - y[0]);
  return res.map((r) => r[1]);
}

// Even-odd ray-cast point-in-polygon; ring is [[lat,lon], ...].
function pointInRing(ring, lat, lon) {
  let inside = false;
  const m = ring.length;
  let j = m - 1;
  for (let i = 0; i < m; i++) {
    const yi = ring[i][0], xi = ring[i][1];
    const yj = ring[j][0], xj = ring[j][1];
    if ((yi > lat) !== (yj > lat)) {
      const xint = (xj - xi) * (lat - yi) / (yj - yi) + xi;
      if (lon < xint) inside = !inside;
    }
    j = i;
  }
  return inside;
}

// Close a boundary-to-boundary sub-chain p (points [lat,lon]) into a ring
// enclosing the SEA side (right of the coastline direction). Returns the ring
// or null if degenerate.
// Assemble SEA rings for the box [s,w,n,e] the osmcoastline way: clip every
// chain to the box into boundary-to-boundary sub-chains, then trace rings by
// following each coast forward (OSM: water on the right) and, at its exit,
// walking the box boundary CLOCKWISE (interior on the right = water) to the
// NEXT coast's entry. Uses the real topology, so a peninsula never encloses
// land. Returns rings as [[lat,lon], ...]. Must match build_map.py.
export function regionSeaPolygons(chains, s, w, n, e) {
  const subs = [];
  for (const chain of chains) {
    for (const [pts, sb, eb] of clipChain(chain, s, w, n, e)) {
      if (sb && eb && pts.length >= 2) subs.push(pts);
    }
  }
  if (subs.length === 0) return [];
  const ww = e - w, hh = n - s, total = 2 * ww + 2 * hh;
  const entries = subs.map((sub) => perimPos(sub[0], s, w, n, e));
  const exits = subs.map((sub) => perimPos(sub[sub.length - 1], s, w, n, e));
  const used = new Array(subs.length).fill(false);
  const rings = [];
  for (let start = 0; start < subs.length; start++) {
    if (used[start]) continue;
    const ring = [];
    let i = start;
    let guard = 0;
    while (!used[i] && guard < 4 * subs.length + 8) {
      guard++;
      used[i] = true;
      for (const p of subs[i]) ring.push(p); // coast A->B (water right)
      const ex = exits[i];
      let best = -1, bestGap = Infinity;
      for (let j = 0; j < subs.length; j++) {
        let gap = ((ex - entries[j]) % total + total) % total; // CW ex->entry
        if (gap <= 1e-12) gap += total; // not the same point
        if (gap < bestGap) { bestGap = gap; best = j; }
      }
      for (const p of closing(ex, entries[best], s, w, n, e, false)) ring.push(p);
      i = best;
    }
    if (ring.length >= 3) rings.push(ring);
  }
  return rings;
}

// Turn assembled coastline chains into projected, decimated, int16 SEA
// polygons for the tile [s,w,n,e] — same encoding as natural=water polys.
export function coastlinePolys(chains, s, w, n, e, lon0, lat0, kx, ky, simplifyM) {
  const out = [];
  for (const ring of regionSeaPolygons(chains, s, w, n, e)) {
    const m = decimate(ring.map(([lat, lon]) => [(lon - lon0) * kx, (lat - lat0) * ky]), simplifyM);
    if (m.length < 3) continue;
    const poly = [];
    for (const [x, y] of m) {
      const ix = Math.max(-32000, Math.min(32000, pyRound(x)));
      const iy = Math.max(-32000, Math.min(32000, pyRound(y)));
      poly.push([ix, iy]);
    }
    out.push(poly);
  }
  return out;
}

// Python's int(round(x)): round-half-to-even, then truncate toward zero.
// build_map.py rounds this way, so matching it keeps our bytes identical.
function pyRound(x) {
  const r = Math.round(x);
  // Math.round rounds .5 up; Python rounds .5 to even. Correct the even case.
  if (Math.abs(x - Math.trunc(x)) === 0.5) {
    const f = Math.floor(x);
    return (f % 2 === 0) ? f : f + 1;
  }
  return r;
}

// A tiny growable little-endian byte writer.
class ByteWriter {
  constructor() { this.bytes = []; }
  get length() { return this.bytes.length; }
  u8(v) { this.bytes.push(v & 0xff); }
  ascii(s) { for (let i = 0; i < s.length; i++) this.bytes.push(s.charCodeAt(i) & 0xff); }
  u16(v) { this.u8(v); this.u8(v >> 8); }
  i16(v) { this.u16(v & 0xffff); }
  u32(v) { this.u8(v); this.u8(v >> 8); this.u8(v >> 16); this.u8(v >> 24); }
  i32(v) { this.u32(v >>> 0); }
  f64(v) {
    const b = new Uint8Array(8);
    new DataView(b.buffer).setFloat64(0, v, true);
    for (let i = 0; i < 8; i++) this.bytes.push(b[i]);
  }
  concat(other) { for (const x of other.bytes) this.bytes.push(x); }
  toUint8Array() { return Uint8Array.from(this.bytes); }
}

// Byte size of a header-only (empty) map for a bbox — used to detect "no roads".
export function headerOnlySize(s, w, n, e, tileDeg = TILE_DEG) {
  const td = tileDeg;
  const lat0 = Math.floor(s / td) * td;
  const lon0 = Math.floor(w / td) * td;
  const nx = Math.ceil((e - lon0) / td);
  const ny = Math.ceil((n - lat0) / td);
  return 36 + nx * ny * 8;
}

// Encode an already-fetched Overpass JSON object into an .ebm Uint8Array.
export function buildEbm(json, { s, w, n, e, tileDeg = TILE_DEG, simplifyM = SIMPLIFY_M }) {
  const nodes = new Map();
  const ways = [];
  const waterWays = []; // node-id lists for natural=water polygons
  const coastWays = []; // node-id lists for natural=coastline ways
  for (const el of json.elements) {
    if (el.type === "node" && el.lat != null && el.lon != null) {
      nodes.set(el.id, [el.lat, el.lon]);
    } else if (el.type === "way" && el.nodes) {
      const tags = el.tags || {};
      const cls = classify(tags);
      if (cls != null) ways.push([cls, el.nodes]);
      else if (tags.natural === "water") waterWays.push(el.nodes);
      else if (tags.natural === "coastline") coastWays.push(el.nodes);
    }
  }
  const coastChains = assembleCoastline(coastWays, nodes);

  const td = tileDeg;
  const midLat = (s + n) / 2;
  const kx = 111320.0 * Math.cos((midLat * Math.PI) / 180); // m per deg lon
  const ky = 110540.0;                                      // m per deg lat
  const lat0 = Math.floor(s / td) * td;
  const lon0 = Math.floor(w / td) * td;
  const nx = Math.ceil((e - lon0) / td);
  const ny = Math.ceil((n - lat0) / td);
  if (nx <= 0 || ny <= 0) throw new Error("empty area");

  const tileWm = td * kx, tileHm = td * ky;
  const tiles = new Map(); // ty*nx+tx -> [[cls, [[x,y],…]], …]

  const tileOf = (p) => [Math.floor(p[0] / tileWm), Math.floor(p[1] / tileHm)];

  const emit = (tx, ty, cls, run) => {
    if (run.length < 2 || tx < 0 || tx >= nx || ty < 0 || ty >= ny) return;
    const ox = tx * tileWm, oy = ty * tileHm;
    const pts = [];
    for (const [x, y] of run) {
      const lx = Math.max(-32000, Math.min(32000, pyRound(x - ox)));
      const ly = Math.max(-32000, Math.min(32000, pyRound(y - oy)));
      const last = pts[pts.length - 1];
      if (last && last[0] === lx && last[1] === ly) continue;
      pts.push([lx, ly]);
    }
    if (pts.length >= 2) {
      const key = ty * nx + tx;
      if (!tiles.has(key)) tiles.set(key, []);
      tiles.get(key).push([cls, pts]);
    }
  };

  for (const [cls, nids] of ways) {
    const pts = [];
    for (const id of nids) { const p = nodes.get(id); if (p) pts.push(p); }
    if (pts.length < 2) continue;
    const m = rdp(pts.map(([lat, lon]) => [(lon - lon0) * kx, (lat - lat0) * ky]), simplifyM);
    if (m.length < 2) continue;
    let run = [m[0]];
    let cur = tileOf(m[0]);
    for (let i = 1; i < m.length; i++) {
      const p = m[i];
      const t = tileOf(p);
      run.push(p);
      if (t[0] !== cur[0] || t[1] !== cur[1]) {
        emit(cur[0], cur[1], cls, run);
        run = [run[run.length - 2], p];
        cur = t;
      }
    }
    emit(cur[0], cur[1], cls, run);
  }

  // Water polygons (natural=water) -> WTR2 section. Points are metres E/N of
  // the grid SW origin (lat0,lon0) — the same snapped origin the road grid
  // uses. A polygon is kept if any of its points fall in [s,w,n,e]; the whole
  // simplified ring is stored (no clipping).
  const waterPolys = [];
  for (const nids of waterWays) {
    const pts = [];
    for (const id of nids) { const p = nodes.get(id); if (p) pts.push(p); }
    let inBox = false;
    for (const [lat, lon] of pts) {
      if (lat >= s && lat <= n && lon >= w && lon <= e) { inBox = true; break; }
    }
    if (!inBox) continue;
    // Radial decimation (NOT RDP) so closed rings survive; the implicit
    // closing point (equal to the first) drops at distance 0. The device
    // closes the ring, so we never append a closing point.
    const m = decimate(pts.map(([lat, lon]) => [(lon - lon0) * kx, (lat - lat0) * ky]), simplifyM);
    if (m.length < 3) continue;
    const poly = [];
    for (const [x, y] of m) {
      const ix = Math.max(-32000, Math.min(32000, pyRound(x)));
      const iy = Math.max(-32000, Math.min(32000, pyRound(y)));
      poly.push([ix, iy]);
    }
    waterPolys.push(poly);
  }

  // Coastline sea-fill: region-level assembly (osmcoastline-style) so a
  // peninsula's coast never encloses land — see regionSeaPolygons().
  for (const poly of coastlinePolys(coastChains, s, w, n, e, lon0, lat0, kx, ky, simplifyM)) {
    waterPolys.push(poly);
  }

  // Serialize header.
  const out = new ByteWriter();
  out.ascii("EBM2");
  out.f64(lat0); out.f64(lon0); out.f64(td);
  out.i32(nx); out.i32(ny);

  // Build each tile blob, then the index, then concat.
  const blobs = new Map();
  for (const [key, polys] of tiles) {
    const b = new ByteWriter();
    b.u16(Math.min(polys.length, 0xffff));
    for (const [cls, pts] of polys) {
      b.u8(cls);
      b.u16(Math.min(pts.length, 0xffff));
      for (const [x, y] of pts) { b.i16(x); b.i16(y); }
    }
    blobs.set(key, b);
  }

  let off = 36 + nx * ny * 8;
  const index = new ByteWriter();
  const ordered = [];
  for (let ty = 0; ty < ny; ty++) {
    for (let tx = 0; tx < nx; tx++) {
      const b = blobs.get(ty * nx + tx);
      if (b && b.length) {
        index.u32(off); index.u32(b.length);
        ordered.push(b); off += b.length;
      } else {
        index.u32(0); index.u32(0);
      }
    }
  }
  out.concat(index);
  for (const b of ordered) out.concat(b);

  // WTR2 water section (after the tile data).
  out.ascii("WTR2");
  out.u16(waterPolys.length & 0xffff);
  for (const poly of waterPolys) {
    out.u16(poly.length & 0xffff);
    for (const [x, y] of poly) { out.i16(x); out.i16(y); }
  }
  return out.toUint8Array();
}

// Fetch raw Overpass JSON for a bbox, rotating through the mirrors with a
// retry pass. `onStatus(text)` reports which mirror is being tried.
export async function fetchOverpass({ s, w, n, e }, onStatus = () => {}) {
  const bbox = { S: String(s), W: String(w), N: String(n), E: String(e) };
  const q = QUERY.replace(/%S/g, bbox.S).replace(/%W/g, bbox.W)
                 .replace(/%N/g, bbox.N).replace(/%E/g, bbox.E);
  const body = "data=" + encodeURIComponent(q);

  const total = OVERPASS_ENDPOINTS.length * 2;
  let lastStatus = 0, lastError = null;
  for (let attempt = 0; attempt < total; attempt++) {
    const url = OVERPASS_ENDPOINTS[attempt % OVERPASS_ENDPOINTS.length];
    let host = url;
    try { host = new URL(url).host; } catch (_) {}
    onStatus(`server ${host} (try ${attempt + 1}/${total})`);
    try {
      const resp = await fetch(url, {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body,
      });
      if (resp.status === 200) return await resp.json();
      lastStatus = resp.status;
    } catch (err) {
      lastError = err;
    }
    await new Promise((r) => setTimeout(r, 800));
  }
  if (lastStatus === 429) throw new Error("map servers are rate-limiting — wait a minute and retry");
  if (lastStatus === 504 || lastStatus >= 500) throw new Error(`map servers are busy (${lastStatus}) — try again, or draw a smaller area`);
  if (lastError) throw new Error(lastError.message || String(lastError));
  throw new Error("no response from map servers");
}
