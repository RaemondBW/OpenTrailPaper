// Region polygons (Geofabrik GeoJSON geometry) with fast point-in-polygon and
// "is this whole rectangle inside" tests, for deciding which extract builds
// which cell. Even-odd rule over every ring, so holes work.
const G = 0.1;   // edge bucket size, degrees

export class Region {
  // geometry: GeoJSON Polygon or MultiPolygon ([lon, lat] positions).
  constructor(geometry) {
    const polys = geometry.type === "Polygon" ? [geometry.coordinates] : geometry.coordinates;
    const edges = [];   // [lat1, lon1, lat2, lon2]
    let s = 90, w = 180, n = -90, e = -180;
    for (const poly of polys) {
      for (const ring of poly) {
        for (let i = 0; i < ring.length; i++) {
          const [lo1, la1] = ring[i];
          const [lo2, la2] = ring[(i + 1) % ring.length];
          if (lo1 === lo2 && la1 === la2) continue;
          edges.push([la1, lo1, la2, lo2]);
          s = Math.min(s, la1); n = Math.max(n, la1); w = Math.min(w, lo1); e = Math.max(e, lo1);
        }
      }
    }
    this.bbox = { s, w, n, e };
    this.polys = polys;
    this.edges = edges;
    // Bucket edges by grid cell they overlap (by bbox).
    this.gx0 = Math.floor(w / G); this.gy0 = Math.floor(s / G);
    this.gnx = Math.floor(e / G) - this.gx0 + 1; this.gny = Math.floor(n / G) - this.gy0 + 1;
    this.grid = new Map();
    this.rows = new Map();   // lat band -> edges spanning it (for ray casting)
    edges.forEach((ed, k) => {
      const [la1, lo1, la2, lo2] = ed;
      const x0 = Math.floor(Math.min(lo1, lo2) / G), x1 = Math.floor(Math.max(lo1, lo2) / G);
      const y0 = Math.floor(Math.min(la1, la2) / G), y1 = Math.floor(Math.max(la1, la2) / G);
      for (let y = y0; y <= y1; y++) {
        let r = this.rows.get(y); if (!r) this.rows.set(y, (r = [])); r.push(k);
        for (let x = x0; x <= x1; x++) {
          const key = y * 100000 + x;
          let b = this.grid.get(key); if (!b) this.grid.set(key, (b = [])); b.push(k);
        }
      }
    });
  }

  containsPoint(lat, lon) {
    const b = this.bbox;
    if (lat < b.s || lat > b.n || lon < b.w || lon > b.e) return false;
    const row = this.rows.get(Math.floor(lat / G));
    if (!row) return false;
    let inside = false;
    for (const k of row) {
      const [yi, xi, yj, xj] = this.edges[k];
      if ((yi > lat) !== (yj > lat)) {
        const xint = (xj - xi) * (lat - yi) / (yj - yi) + xi;
        if (lon < xint) inside = !inside;
      }
    }
    return inside;
  }

  // True when the whole rectangle lies inside the region: its corners are
  // inside and no boundary edge touches it.
  containsRect(s, w, n, e) {
    const b = this.bbox;
    if (s < b.s || n > b.n || w < b.w || e > b.e) return false;
    if (!this.containsPoint(s, w) || !this.containsPoint(s, e) ||
        !this.containsPoint(n, w) || !this.containsPoint(n, e)) return false;
    const seen = new Set();
    for (let y = Math.floor(s / G); y <= Math.floor(n / G); y++) {
      for (let x = Math.floor(w / G); x <= Math.floor(e / G); x++) {
        const bucket = this.grid.get(y * 100000 + x);
        if (!bucket) continue;
        for (const k of bucket) {
          if (seen.has(k)) continue;
          seen.add(k);
          const [la1, lo1, la2, lo2] = this.edges[k];
          if (segTouchesRect(lo1, la1, lo2, la2, s, w, n, e)) return false;
        }
      }
    }
    return true;
  }

  // True when the rectangle and the region share any point: an edge touches
  // the rectangle, or the rectangle lies wholly inside the region.
  intersectsRect(s, w, n, e) {
    const b = this.bbox;
    if (n < b.s || s > b.n || e < b.w || w > b.e) return false;
    for (let y = Math.floor(s / G); y <= Math.floor(n / G); y++) {
      for (let x = Math.floor(w / G); x <= Math.floor(e / G); x++) {
        const bucket = this.grid.get(y * 100000 + x);
        if (!bucket) continue;
        for (const k of bucket) {
          const [la1, lo1, la2, lo2] = this.edges[k];
          if (segTouchesRect(lo1, la1, lo2, la2, s, w, n, e)) return true;
        }
      }
    }
    return this.containsPoint(s, w);
  }

  // Bounding boxes of the outer rings, one per polygon part.
  partBoxes() {
    return this.polys.map((poly) => {
      let s = 90, w = 180, n = -90, e = -180;
      for (const [lo, la] of poly[0]) { s = Math.min(s, la); n = Math.max(n, la); w = Math.min(w, lo); e = Math.max(e, lo); }
      return { s, w, n, e };
    });
  }
}

function segTouchesRect(ax, ay, bx, by, s, w, n, e) {
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

export function bboxGeometry(s, w, n, e) {
  return { type: "Polygon", coordinates: [[[w, s], [e, s], [e, n], [w, n], [w, s]]] };
}
