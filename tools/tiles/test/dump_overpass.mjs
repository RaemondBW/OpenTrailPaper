// Equivalence test, step 1: write the Overpass responses a phone would get
// when downloading each cell ALONE, derived from the same extract the builder
// used (same snapshot), as the raw JSON files the apps parse:
//
//   <dir>/<id>.map.json    MapBuilder.query over the batch box (cell ± 0.003°)
//   <dir>/<id>.coast.json  MapBuilder.fetchCoastline over that box ± 0.35°
//   <dir>/<id>.poi.json    MapBuilder.poiQuery over the cell ± 0.003°
//   <dir>/<id>.elev.txt    the 400 ELV1 values (DEM, as Open-Meteo answers)
//   <dir>/tiles.txt        "<id> <s> <w> <n> <e>" (h3tool, the apps' H3 C)
//
// It also checks coastline.mjs against mapgen.assembleCoastline on every
// cell's real coastline input.
//
//   node dump_overpass.mjs <pbf> <s,w,n,e> <dir> [--dem-cache d] [--cells a,b]
import fs from "node:fs";
import path from "node:path";
import { assembleCoastline } from "../../../docs/mapgen.js";
import { loadOsm } from "../osmstore.mjs";
import { OverpassEmu, poiResponse } from "../overpass.mjs";
import { assembleCoastlineFast } from "../coastline.mjs";
import { mapBox, seaBox, elevationSamplePoints, elevationValue } from "../apptile.mjs";
import { Dem } from "../dem.mjs";
import { cellBboxes, cellsAt } from "../build_region.mjs";
import { polygonToCells } from "h3-js";
import { Region, bboxGeometry } from "../poly.mjs";

const [pbf, box, dir, ...rest] = process.argv.slice(2);
const opt = {};
for (let i = 0; i < rest.length; i += 2) opt[rest[i].replace(/^--/, "")] = rest[i + 1];
fs.mkdirSync(dir, { recursive: true });
const [s, w, n, e] = box.split(",").map(Number);
const region = new Region(bboxGeometry(s, w, n, e));
let ids = polygonToCells(region.polys[0], 6, true).sort();
let tiles = cellBboxes(ids).filter((t) => { const B = mapBox(t); return region.containsRect(B.s, B.w, B.n, B.e); });
if (opt.cells) { const want = new Set(opt.cells.split(",")); tiles = tiles.filter((t) => want.has(t.id)); }

const store = await loadOsm(path.resolve(pbf), { tmpDir: path.join(dir, ".tmp"), log: (m) => console.error(m) });
const emu = new OverpassEmu(store);
const dem = new Dem({ cacheDir: opt["dem-cache"] || path.join(dir, ".dem") });
const poiCells = cellsAt(store.pois.map((p) => [p.lat, p.lon]));

const sameChains = (a, b) => a.length === b.length && a.every((c, i) => c.length === b[i].length && c.every((p, k) => p[0] === b[i][k][0] && p[1] === b[i][k][1]));
let coastChecked = 0;
for (const t of tiles) {
  fs.writeFileSync(path.join(dir, `${t.id}.map.json`), JSON.stringify(emu.mapResponse(mapBox(t))));
  // Coastline response: `(._;>;); out body;` prints nodes then ways, by id.
  const { coastWays, nodes } = emu.coast(seaBox(t));
  const nodeIds = [...nodes.keys()].sort((x, y) => x - y);
  fs.writeFileSync(path.join(dir, `${t.id}.coast.json`), JSON.stringify({
    elements: [
      ...nodeIds.map((id) => ({ type: "node", id, lat: nodes.get(id)[0], lon: nodes.get(id)[1] })),
      // emu.coast returns the ways in id order; the ids themselves are unused.
      ...coastWays.map((nids, k) => ({ type: "way", id: k + 1, nodes: nids, tags: { natural: "coastline" } })),
    ],
  }));
  if (coastWays.length) {
    if (!sameChains(assembleCoastline(coastWays, nodes), assembleCoastlineFast(coastWays, nodes))) {
      console.error(`COASTLINE MISMATCH in ${t.id}`); process.exitCode = 1;
    }
    coastChecked++;
  }
  // POIs: the app fetches the box and keeps those whose H3 cell is this one;
  // other POIs in the box are irrelevant to this tile, but include them so the
  // Swift side does the cell test itself.
  const B = mapBox(t);
  const inBox = store.pois.filter((p, i) => (p.lat >= B.s && p.lat <= B.n && p.lon >= B.w && p.lon <= B.e) || poiCells[i] === t.id);
  fs.writeFileSync(path.join(dir, `${t.id}.poi.json`), JSON.stringify(poiResponse(inBox)));
  const grid = [];
  for (const [la, lo] of elevationSamplePoints(t.s, t.w, t.n, t.e)) grid.push(elevationValue(await dem.at(la, lo)));
  fs.writeFileSync(path.join(dir, `${t.id}.elev.txt`), grid.join(" ") + "\n");
}
fs.writeFileSync(path.join(dir, "tiles.txt"), tiles.map((t) => `${t.id} ${t.s} ${t.w} ${t.n} ${t.e}`).join("\n") + "\n");
console.error(`dumped ${tiles.length} cells; coastline join identical to mapgen on ${coastChecked} cells with coast`);
