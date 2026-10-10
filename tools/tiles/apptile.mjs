// The phone apps' finished tile, in JS.
//
// docs/mapgen.js is the reference for the ROAD part of a tile (EBM2 header,
// sub-tile index, polylines and way-flag trailers) and for .poi files; the iOS
// and Android MapBuilders match it byte for byte (tools/map_test/
// run_crossport.sh). What the apps then append per tile is their own code,
// which mapgen.js does not have (the website writes water/parks its own way
// and no elevation):
//
//   MapBuilder.buildBatch (companion-ios/Sources/MapBuilder.swift, and
//   MapBuilder.kt): roads ++ [ELV1] ++ WTR2 (natural=water + clipped sea
//   rings) ++ PRK2, then drop the tile if it is header-only.
//
// This file ports exactly that appendix — appendElevation, appendWater
// (with clipToBox), appendParks, isEmpty — so the server can write the bytes
// the apps write. Rounding is Swift's `.rounded()` (half away from zero),
// which the Kotlin port reproduces (MapBuilder.rnd), NOT mapgen's pyRound.
//
// Keep in step with MapBuilder.swift / MapBuilder.kt; tools/tiles/test/
// equivalence.sh compares this against the Swift code itself.
import { buildEbm, decimate, isPark, regionSeaPolygons, TILE_DEG, SIMPLIFY_M } from "../../docs/mapgen.js";

export const ELEVATION_GRID = 20;   // MapBuilder.elevationGrid

// Swift Int(x.rounded()): round half away from zero.
export function swiftRound(x) {
  return x < 0 ? -Math.round(-x) : Math.round(x);
}
const clampI16 = (v) => Math.max(-32000, Math.min(32000, v));

class Bytes {
  constructor() { this.parts = []; this.len = 0; }
  push(u8) { this.parts.push(u8); this.len += u8.length; }
  ascii(s) { this.push(Uint8Array.from(s, (c) => c.charCodeAt(0))); }
  u16(v) { this.push(Uint8Array.of(v & 0xff, (v >> 8) & 0xff)); }
  i16(v) { this.u16(v & 0xffff); }
  i32(v) { const b = new Uint8Array(4); new DataView(b.buffer).setInt32(0, v, true); this.push(b); }
  f64(v) { const b = new Uint8Array(8); new DataView(b.buffer).setFloat64(0, v, true); this.push(b); }
  toUint8Array() {
    const out = new Uint8Array(this.len);
    let o = 0;
    for (const p of this.parts) { out.set(p, o); o += p.length; }
    return out;
  }
}

// End of the road data in an EBM2 blob (header + index + sub-tiles): what the
// apps' encode() returns. Same as tools/map_test/crossport/crossport.mjs.
export function roadsEnd(b) {
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const nx = dv.getInt32(28, true), ny = dv.getInt32(32, true);
  let end = 36 + nx * ny * 8;
  for (let k = 0; k < nx * ny; k++) {
    const off = dv.getUint32(36 + k * 8, true), len = dv.getUint32(40 + k * 8, true);
    if (off && off + len > end) end = off + len;
  }
  return end;
}

export function headerOnly(s, w, n, e) {
  const td = TILE_DEG;
  const lat0 = Math.floor(s / td) * td;
  const lon0 = Math.floor(w / td) * td;
  const nx = Math.ceil((e - lon0) / td);
  const ny = Math.ceil((n - lat0) / td);
  return 36 + nx * ny * 8;
}

// MapBuilder.extractWaterWays / extractParkWays: every way in the response
// with the tag, in element order, its nodes resolved (missing ones skipped).
export function fillWays(json) {
  const nodes = new Map();
  for (const el of json.elements) {
    if (el.type === "node" && el.lat != null && el.lon != null) nodes.set(el.id, [el.lat, el.lon]);
  }
  const water = [], parks = [];
  for (const el of json.elements) {
    if (el.type !== "way" || !el.nodes) continue;
    const t = el.tags;
    const resolve = () => { const p = []; for (const id of el.nodes) { const q = nodes.get(id); if (q) p.push(q); } return p; };
    if (t && t.natural === "water") water.push(resolve());
    if (t && isPark(t)) parks.push(resolve());
  }
  return { water, parks };
}

// MapBuilder.clipToBox: Sutherland–Hodgman against [s,w,n,e]; points [lat,lon].
export function clipToBox(poly, s, w, n, e) {
  const clip = (pts, inside, isect) => {
    if (pts.length === 0) return [];
    const res = [];
    const m = pts.length;
    for (let i = 0; i < m; i++) {
      const cur = pts[i], prev = pts[(i + m - 1) % m];
      const curIn = inside(cur), prevIn = inside(prev);
      if (curIn) {
        if (!prevIn) res.push(isect(prev, cur));
        res.push(cur);
      } else if (prevIn) {
        res.push(isect(prev, cur));
      }
    }
    return res;
  };
  let p = poly;
  p = clip(p, (q) => q[1] >= w, (a, b) => { const t = (w - a[1]) / (b[1] - a[1]); return [a[0] + t * (b[0] - a[0]), w]; });
  p = clip(p, (q) => q[1] <= e, (a, b) => { const t = (e - a[1]) / (b[1] - a[1]); return [a[0] + t * (b[0] - a[0]), e]; });
  p = clip(p, (q) => q[0] >= s, (a, b) => { const t = (s - a[0]) / (b[0] - a[0]); return [s, a[1] + t * (b[1] - a[1])]; });
  p = clip(p, (q) => q[0] <= n, (a, b) => { const t = (n - a[0]) / (b[0] - a[0]); return [n, a[1] + t * (b[1] - a[1])]; });
  return p;
}

function projector(s, w, n) {
  const td = TILE_DEG;
  const midLat = (s + n) / 2;
  const kx = 111320.0 * Math.cos(midLat * Math.PI / 180);
  const ky = 110540.0;
  const lat0 = Math.floor(s / td) * td;
  const lon0 = Math.floor(w / td) * td;
  return (pts) => pts.map(([lat, lon]) => [(lon - lon0) * kx, (lat - lat0) * ky]);
}

function quantise(m) {
  return m.map(([x, y]) => [clampI16(swiftRound(x)), clampI16(swiftRound(y))]);
}

function writeFill(out, magic, polys) {
  out.ascii(magic);
  out.u16(Math.min(polys.length, 0xffff));
  for (const poly of polys) {
    out.u16(Math.min(poly.length, 0xffff));
    for (const [x, y] of poly) { out.i16(x); out.i16(y); }
  }
}

const inBox = (pts, s, w, n, e) => pts.some(([lat, lon]) => lat >= s && lat <= n && lon >= w && lon <= e);

// MapBuilder.appendWater
export function waterSection(out, waterWays, seaRings, s, w, n, e) {
  const proj = projector(s, w, n);
  const polys = [];
  for (const pts of waterWays) {
    if (!inBox(pts, s, w, n, e)) continue;
    const m = decimate(proj(pts), SIMPLIFY_M);
    if (m.length < 3) continue;
    polys.push(quantise(m));
  }
  for (const ring of seaRings) {
    const clipped = clipToBox(ring, s, w, n, e);
    if (clipped.length < 3) continue;
    const m = decimate(proj(clipped), SIMPLIFY_M);
    if (m.length < 3) continue;
    polys.push(quantise(m));
  }
  writeFill(out, "WTR2", polys);
}

// MapBuilder.appendParks
export function parkSection(out, parkWays, s, w, n, e) {
  const proj = projector(s, w, n);
  const polys = [];
  for (const pts of parkWays) {
    if (!inBox(pts, s, w, n, e)) continue;
    const m = decimate(proj(pts), SIMPLIFY_M);
    if (m.length < 3) continue;
    polys.push(quantise(m));
  }
  writeFill(out, "PRK2", polys);
}

// MapBuilder.appendElevation. grid: gridN*gridN int16 metres, row 0 = south.
export function elevationSection(out, grid, s, w, n, e, gridN = ELEVATION_GRID) {
  if (!grid || grid.length !== gridN * gridN) return;
  out.ascii("ELV1");
  out.i32(gridN); out.i32(gridN);
  out.f64(s); out.f64(w); out.f64(n); out.f64(e);
  for (const v of grid) out.i16(v);
}

// The points MapBuilder.fetchElevationGrid sends to Open-Meteo, in order:
// row-major from the south-west corner, formatted "%.5f" on the wire.
export function elevationSamplePoints(s, w, n, e, gridN = ELEVATION_GRID) {
  const pts = [];
  for (let i = 0; i < gridN; i++) {
    const lat = s + (n - s) * i / (gridN - 1);
    for (let j = 0; j < gridN; j++) {
      const lon = w + (e - w) * j / (gridN - 1);
      pts.push([Number(lat.toFixed(5)), Number(lon.toFixed(5))]);
    }
  }
  return pts;
}

// Elevation metres (number, or null for "no data") -> the int16 the app
// stores: Int16(max(-2000, min(9000, (ev ?? 0).rounded()))).
export function elevationValue(ev) {
  return Math.max(-2000, Math.min(9000, swiftRound(ev == null || Number.isNaN(ev) ? 0 : ev)));
}

// The sea rings a single-hex download computes: one coastline fetch over the
// selection's union bbox (cell bbox padded 0.003°) padded by a further 0.35°,
// rings assembled against that padded box (MapsView.download).
export function seaBox(t) {
  const u = { s: t.s - 0.003, w: t.w - 0.003, n: t.n + 0.003, e: t.e + 0.003 };
  const pad = 0.35;
  return { s: u.s - pad, w: u.w - pad, n: u.n + pad, e: u.e + pad };
}
// The map batch bbox for a single-hex download (MapBuilder.buildBatch).
export function mapBox(t) {
  const pad = 0.003;
  return { s: t.s - pad, w: t.w - pad, n: t.n + pad, e: t.e + pad };
}

export function seaRingsFor(chains, t) {
  const P = seaBox(t);
  return regionSeaPolygons(chains, P.s, P.w, P.n, P.e);
}

// One finished app tile for cell t = {id, s, w, n, e}:
//   mapJson   — the map query's response for mapBox(t)
//   seaRings  — seaRingsFor(coastline chains over seaBox(t), t)
//   elevation — int16[400] or null (the app ships a tile without ELV1 when
//               Open-Meteo fails)
// Returns the bytes, or null when the app would drop the tile as empty.
export function buildAppTile(mapJson, seaRings, elevation, t) {
  const { s, w, n, e } = t;
  const ebm = buildEbm(mapJson, { s, w, n, e });
  const out = new Bytes();
  out.push(ebm.subarray(0, roadsEnd(ebm)));
  elevationSection(out, elevation, s, w, n, e);
  const { water, parks } = fillWays(mapJson);
  waterSection(out, water, seaRings, s, w, n, e);
  parkSection(out, parks, s, w, n, e);
  const bytes = out.toUint8Array();
  return bytes.length <= headerOnly(s, w, n, e) ? null : bytes;
}
