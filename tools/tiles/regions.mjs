// Which region builds which cell — a pure function of the region list, so
// every job of a run (and every run) agrees without talking to the others.
//
//   owner(cell)  = the FIRST region, in regions.json order, whose polygon
//                  contains the cell's centre (H3 centroid). Every cell whose
//                  centre lies in any extract has exactly one owner; cells in
//                  open ocean beyond every extract have none (the apps fall
//                  back to Overpass there, which returns nothing anyway).
//   interior     = the owner's polygon also contains the cell's whole map
//                  fetch box (cell bbox + 0.003°): every way the app's query
//                  would return is in the owner's extract (Geofabrik cuts
//                  with complete ways), so the owner builds it alone.
//   border       = everything else: built in the border phase from "strips"
//                  — the slices of every extract whose polygon reaches the
//                  cell (see strip.mjs) — merged into one snapshot.
//   deferred     = interior, but the owner's extract cannot settle the sea
//                  fill alone: the cell's coastline box (seaBox, +0.353°)
//                  reaches into ANOTHER extract (seaOut) and the owner sees
//                  coastline in it, or sees nothing at all (open water whose
//                  coast may be the neighbour's). The apps assemble sea rings
//                  from every coastline way in that box, so a coast just over
//                  the border changes the rings. Decided per cell after the
//                  owner has looked at its data (build_region.mjs); deferred
//                  cells join the border phase.
//
// Geofabrik polygons are buffered a little past the real border, but a cell
// is ~7 km across, so roughly 5-15% of a country's cells end up "border".
import fs from "node:fs";
import { polygonToCells, cellToLatLng } from "h3-js";
import { Region } from "./poly.mjs";
import { mapBox, seaBox } from "./apptile.mjs";

// regions.json: { regions: [{ id, url, geometry, ... }] } in ownership order.
export function loadRegions(file) {
  const j = typeof file === "string" ? JSON.parse(fs.readFileSync(file, "utf8")) : file;
  return j.regions.map((r, k) => ({ ...r, k, poly: new Region(r.geometry) }));
}

export function ownerOf(regions, lat, lon) {
  for (const r of regions) {
    const b = r.poly.bbox;
    if (lat < b.s || lat > b.n || lon < b.w || lon > b.e) continue;
    if (r.poly.containsPoint(lat, lon)) return r.k;
  }
  return -1;
}

// Rectangles covering `box` grown by the reach of a cell's coastline fetch
// (seaBox: bbox + 0.353°) plus a cell's own half-size, split so that none is
// wider than 90° (H3 treats wider polygons as crossing the antimeridian).
function searchRects(box) {
  const maxLat = Math.min(89, Math.max(Math.abs(box.s), Math.abs(box.n)) + 0.5);
  const latM = 0.5, lonM = Math.min(10, 0.4 + 0.08 / Math.cos(maxLat * Math.PI / 180));
  const s = Math.max(-89.9, box.s - latM), n = Math.min(89.9, box.n + latM);
  const w = Math.max(-180, box.w - lonM), e = Math.min(180, box.e + lonM);
  const out = [];
  for (let x = w; x < e; x += 90) out.push({ s, w: x, n, e: Math.min(e, x + 90) });
  return out;
}

function cellsInRect(r, depth = 0) {
  const ring = [[r.s, r.w], [r.s, r.e], [r.n, r.e], [r.n, r.w], [r.s, r.w]];
  try {
    return polygonToCells(ring, 6, false);
  } catch (e) {
    // H3 gives up (E_FAILED) on some big polar rectangles — Antarctica's
    // 90°-wide slices down to -89.9° crashed the whole job and every region
    // batched with it. Split into quarters and retry; past 6 levels (a
    // ~1.4° x 0.3° rectangle) give the slice up rather than the job.
    if (depth >= 6) {
      console.error(`regions: H3 polygonToCells failed on ${JSON.stringify(r)}; skipped`);
      return [];
    }
    const ml = (r.s + r.n) / 2, mo = (r.w + r.e) / 2;
    const out = [];
    for (const q of [{ s: r.s, w: r.w, n: ml, e: mo }, { s: r.s, w: mo, n: ml, e: r.e },
                     { s: ml, w: r.w, n: r.n, e: mo }, { s: ml, w: mo, n: r.n, e: r.e }]) {
      for (const c of cellsInRect(q, depth + 1)) out.push(c);
    }
    return out;
  }
}

// Every cell whose coastline fetch box (seaBox) touches region k's polygon,
// classified. `bboxOf(ids)` -> [{id,s,w,n,e}] must be the apps' H3 bbox
// (build_region.cellBboxes). Returns [{ id, s, w, n, e, owner, interior }].
export function cellsNear(regions, k, bboxOf) {
  const R = regions[k];
  const ids = new Set();
  for (const box of R.poly.partBoxes()) {
    for (const rect of searchRects(box)) {
      // The margins in searchRects exceed a cell's reach (seaBox pad plus
      // half a cell), so centroid-in-rect finds every cell that matters.
      for (const c of cellsInRect(rect)) ids.add(c);
    }
  }
  const out = [];
  for (const t of bboxOf([...ids].sort())) {
    const S = seaBox(t);
    if (!R.poly.intersectsRect(S.s, S.w, S.n, S.e)) continue;
    const [lat, lon] = cellToLatLng(t.id);
    const owner = ownerOf(regions, lat, lon);
    let interior = false, seaOut = false;
    if (owner >= 0) {
      const B = mapBox(t), O = regions[owner].poly;
      interior = O.containsRect(B.s, B.w, B.n, B.e);
      // The coastline fetch reaches into another extract, which may hold
      // coastline this one lacks.
      seaOut = !O.containsRect(S.s, S.w, S.n, S.e) &&
        regions.some((r) => r.k !== owner && r.poly.intersectsRect(S.s, S.w, S.n, S.e));
    }
    out.push({ ...t, owner, interior, seaOut });
  }
  return out;
}
