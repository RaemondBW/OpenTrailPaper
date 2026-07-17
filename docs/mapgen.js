// In-browser offline-map builder for the Open E-Paper Bike Computer.
//
// A straight port of tools/maps/build_map.py (and companion-ios MapBuilder.swift)
// so the output is byte-compatible with the .ebm whole-region maps the firmware
// reads from /maps/*.ebm. Fetch OSM ways from Overpass for a bounding box,
// classify + simplify them, clip into the fixed lat/lon tile grid, and encode
// the binary — all locally in the browser, nothing uploaded anywhere.
//
// Format (little-endian):
//   'EBM1', f64 lat0, f64 lon0, f64 tileDeg, i32 nx, i32 ny,
//   index[nx*ny] of (u32 offset, u32 length)  (0,0 = empty tile),
//   tiles: u16 polylineCount, per polyline { u8 class, u16 pointCount,
//          i16 x,y per point (metres E/N of the tile SW corner) }.
//   class: 0 major road, 1 minor road, 2 path, 3 rail.

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
  way["railway"~"^(rail|light_rail)$"](%S,%W,%N,%E);
);
out body;
>;
out skel qt;`;

const MAJOR = new Set(["motorway", "trunk", "primary", "secondary"]);
const MINOR = new Set(["tertiary", "residential", "unclassified", "living_street", "pedestrian"]);
const PATH = new Set(["cycleway", "footway", "path", "track", "steps"]);

// OSM tags -> render class (0 major, 1 minor, 2 path, 3 rail), or null to drop.
// Must agree with build_map.py / MapBuilder.swift.
export function classify(tags) {
  if (tags == null) return null;
  if (tags.railway != null) return 3;
  const hw = tags.highway || "";
  // Sidewalks and crossings duplicate every street — noise on a bike map.
  if (hw === "footway" && (tags.footway === "sidewalk" || tags.footway === "crossing")) return null;
  const base = hw.split("_link")[0];
  if (MAJOR.has(base)) return 0;
  if (MINOR.has(base)) return 1;
  if (PATH.has(base)) return 2;
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
  for (const el of json.elements) {
    if (el.type === "node" && el.lat != null && el.lon != null) {
      nodes.set(el.id, [el.lat, el.lon]);
    } else if (el.type === "way" && el.nodes) {
      const cls = classify(el.tags || {});
      if (cls != null) ways.push([cls, el.nodes]);
    }
  }

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

  // Serialize header.
  const out = new ByteWriter();
  out.ascii("EBM1");
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
