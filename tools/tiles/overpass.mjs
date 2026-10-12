// Answer the apps' Overpass queries from a loaded extract (osmstore.mjs), so
// the unchanged builders (docs/mapgen.js + apptile.mjs) run on exactly the
// JSON a phone would have received for the same OSM snapshot.
//
//   mapResponse(B)   — MapBuilder.query: ways matching the highway / water /
//                      coastline / park filters, plus every member way of a
//                      route=bicycle relation, that intersect B; `out body`
//                      (ways by id), `>; out skel` (their nodes), and the three
//                      derived `bikeroute` elements (member way ids per level).
//   coastWays(P)     — MapBuilder.fetchCoastline: natural=coastline ways in P,
//                      by id, as node-id lists + a node table.
//   POIs are assigned to cells once for the whole extract (build_region.mjs);
//   a cell's POI response is its POIs in Overpass order (nodes, ways,
//   relations, each by id).
//
// Overpass's bbox test for a way: some node inside the box, or some segment
// crossing it. Route levels: a way's highest network over all route=bicycle
// relations it is a member of (icn/ncn 3, rcn 2, anything else 1), which is
// what the query's rel.bk / way(r.rN)(B) sets compute.
import { K_MAP, K_COAST, MISSING } from "./osmstore.mjs";

const E7 = 1e7;

// Liang–Barsky: does segment a->b touch the closed rectangle?
function segHitsBox(ax, ay, bx, by, s, w, n, e) {
  const dx = bx - ax, dy = by - ay;
  const p = [-dx, dx, -dy, dy];
  const q = [ax - w, e - ax, ay - s, n - ay];
  let t0 = 0, t1 = 1;
  for (let i = 0; i < 4; i++) {
    if (p[i] === 0) { if (q[i] < 0) return false; }
    else {
      const t = q[i] / p[i];
      if (p[i] < 0) { if (t > t1) return false; if (t > t0) t0 = t; }
      else { if (t < t0) return false; if (t < t1) t1 = t; }
    }
  }
  return true;
}

export class OverpassEmu {
  // store: loadOsm() result. prebuilt: exportShared() of another instance
  // (worker threads share the index instead of rebuilding it).
  constructor(store, prebuilt = null, { gridDeg = 0.05, coastGridDeg = 0.25 } = {}) {
    this.s = store;
    const nW = store.ways.id.length;
    this.stamp = new Uint32Array(nW);
    this.query = 0;
    if (prebuilt) { Object.assign(this, prebuilt); return; }
    const { ways, refs } = store;
    // Per-way bbox in 1e-7 ints.
    const bb = new Int32Array(nW * 4);
    for (let i = 0; i < nW; i++) {
      let s = 2147483647, w = 2147483647, n = -2147483647, e = -2147483647;
      for (let k = ways.start[i], end = k + ways.len[i]; k < end; k++) {
        const la = refs.lat[k];
        if (la === MISSING) continue;
        const lo = refs.lon[k];
        if (la < s) s = la; if (la > n) n = la; if (lo < w) w = lo; if (lo > e) e = lo;
      }
      bb[i * 4] = s; bb[i * 4 + 1] = w; bb[i * 4 + 2] = n; bb[i * 4 + 3] = e;
    }
    this.bb = bb;
    this.mapIdx = this.#index((i) => (ways.kind[i] & K_MAP) || ways.level[i], gridDeg);
    this.coastIdx = this.#index((i) => ways.kind[i] & K_COAST, coastGridDeg);
  }

  // The index, with its typed arrays passed through `share` (e.g. into
  // SharedArrayBuffers), for `new OverpassEmu(store, exported)`.
  exportShared(share = (x) => x) {
    const idx = (x) => ({ ...x, off: share(x.off), items: share(x.items) });
    return { bb: share(this.bb), mapIdx: idx(this.mapIdx), coastIdx: idx(this.coastIdx) };
  }

  // CSR bucket grid over the ways `want(i)` selects.
  #index(want, deg) {
    const { bb } = this;
    const nW = this.s.ways.id.length;
    const g = Math.round(deg * E7);
    let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    const sel = [];
    for (let i = 0; i < nW; i++) {
      if (!want(i) || bb[i * 4] > bb[i * 4 + 2]) continue;
      sel.push(i);
      const x0 = Math.floor(bb[i * 4 + 1] / g), x1 = Math.floor(bb[i * 4 + 3] / g);
      const y0 = Math.floor(bb[i * 4] / g), y1 = Math.floor(bb[i * 4 + 2] / g);
      if (x0 < minX) minX = x0; if (x1 > maxX) maxX = x1; if (y0 < minY) minY = y0; if (y1 > maxY) maxY = y1;
    }
    if (!sel.length) return { g, minX: 0, minY: 0, nx: 0, ny: 0, off: new Uint32Array(1), items: new Uint32Array(0) };
    const nx = maxX - minX + 1, ny = maxY - minY + 1;
    const off = new Uint32Array(nx * ny + 1);
    const span = (i) => [Math.floor(bb[i * 4 + 1] / g) - minX, Math.floor(bb[i * 4 + 3] / g) - minX,
                         Math.floor(bb[i * 4] / g) - minY, Math.floor(bb[i * 4 + 2] / g) - minY];
    for (const i of sel) {
      const [x0, x1, y0, y1] = span(i);
      for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) off[y * nx + x + 1]++;
    }
    for (let k = 0; k < nx * ny; k++) off[k + 1] += off[k];
    const fill = off.slice(0, nx * ny);
    const items = new Uint32Array(off[nx * ny]);
    for (const i of sel) {
      const [x0, x1, y0, y1] = span(i);
      for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) items[fill[y * nx + x]++] = i;
    }
    return { g, minX, minY, nx, ny, off, items };
  }

  // Way indices (ascending way id) from index `idx` that intersect box B.
  #ways(idx, B) {
    const { bb, stamp } = this;
    const { refs, ways } = this.s;
    if (++this.query === 0xffffffff) { stamp.fill(0); this.query = 1; }
    const q = this.query;
    // Integer prefilter box (inclusive, widened by one unit for safety).
    const S = Math.floor(B.s * E7) - 1, W = Math.floor(B.w * E7) - 1;
    const N = Math.ceil(B.n * E7) + 1, Ea = Math.ceil(B.e * E7) + 1;
    const x0 = Math.max(0, Math.floor(W / idx.g) - idx.minX), x1 = Math.min(idx.nx - 1, Math.floor(Ea / idx.g) - idx.minX);
    const y0 = Math.max(0, Math.floor(S / idx.g) - idx.minY), y1 = Math.min(idx.ny - 1, Math.floor(N / idx.g) - idx.minY);
    const out = [];
    for (let y = y0; y <= y1; y++) {
      for (let x = x0; x <= x1; x++) {
        const k = y * idx.nx + x;
        for (let p = idx.off[k]; p < idx.off[k + 1]; p++) {
          const i = idx.items[p];
          if (stamp[i] === q) continue;
          stamp[i] = q;
          if (bb[i * 4] > N || bb[i * 4 + 2] < S || bb[i * 4 + 1] > Ea || bb[i * 4 + 3] < W) continue;
          // Exact test in degrees, as Overpass: a node inside, or a segment crossing.
          let hit = false, px = 0, py = 0, have = false;
          for (let r = ways.start[i], end = r + ways.len[i]; r < end && !hit; r++) {
            if (refs.lat[r] === MISSING) { have = false; continue; }
            const la = refs.lat[r] / E7, lo = refs.lon[r] / E7;
            if (la >= B.s && la <= B.n && lo >= B.w && lo <= B.e) hit = true;
            else if (have && segHitsBox(px, py, lo, la, B.s, B.w, B.n, B.e)) hit = true;
            px = lo; py = la; have = true;
          }
          if (hit) out.push(i);
        }
      }
    }
    if (this.s.sorted) out.sort((a, b) => a - b);
    else out.sort((a, b) => ways.id[a] - ways.id[b]);
    return out;
  }

  // Way indices the map query / the coastline fetch would return for a box
  // (strip.mjs collects these for the border phase).
  mapWayIdx(B) { return this.#ways(this.mapIdx, B); }
  coastWayIdx(B) { return this.#ways(this.coastIdx, B); }

  // The map query's JSON response for box B.
  mapResponse(B) {
    const { ways, refs, tags } = this.s;
    const idxs = this.#ways(this.mapIdx, B);
    const wayEls = [], nodeEls = [];
    const byLevel = [null, [], [], []];
    for (const i of idxs) {
      const start = ways.start[i], len = ways.len[i];
      const nodes = new Array(len);
      for (let k = 0; k < len; k++) {
        const r = start + k;
        nodes[k] = refs.id[r];
        if (refs.lat[r] !== MISSING) nodeEls.push({ type: "node", id: refs.id[r], lat: refs.lat[r] / E7, lon: refs.lon[r] / E7 });
      }
      wayEls.push({ type: "way", id: ways.id[i], nodes, tags: tags[ways.tag[i]] });
      if (ways.level[i]) byLevel[ways.level[i]].push(ways.id[i]);
    }
    const derived = [3, 2, 1].map((lvl) => ({ type: "bikeroute", id: 4 - lvl,
      tags: { level: String(lvl), ways: byLevel[lvl].join(";") } }));
    return { elements: wayEls.concat(nodeEls, derived) };
  }

  // The coastline fetch for box P: node-id lists (by way id) and node table.
  coast(P) {
    const { ways, refs } = this.s;
    const coastWays = [];
    const nodes = new Map();
    for (const i of this.#ways(this.coastIdx, P)) {
      const start = ways.start[i], len = ways.len[i];
      const nids = new Array(len);
      for (let k = 0; k < len; k++) {
        const r = start + k;
        nids[k] = refs.id[r];
        if (refs.lat[r] !== MISSING) nodes.set(refs.id[r], [refs.lat[r] / E7, refs.lon[r] / E7]);
      }
      coastWays.push(nids);
    }
    return { coastWays, nodes };
  }
}

// A cell's POI response: its POIs as Overpass `out tags center` elements.
export function poiResponse(pois) {
  return {
    elements: pois.map((p) => p.kind === "n"
      ? { type: "node", id: p.id, lat: p.lat, lon: p.lon, tags: p.tags }
      : { type: p.kind === "w" ? "way" : "relation", id: p.id, center: { lat: p.lat, lon: p.lon }, tags: p.tags }),
  };
}
