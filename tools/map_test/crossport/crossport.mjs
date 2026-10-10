// JS side of the cross-port check (run_crossport.sh): the website builder
// docs/mapgen.js is the reference. Writes, per tile, <id>.ebm cut at the end of
// the road data (the part the app builders produce in encode(); water, parks
// and elevation are appended by separate per-port code) and <id>.bbox.poi.
// With h3-js available (H3JS_DIR=<dir containing node_modules/h3-js>) it also
// writes <id>.poi using the H3 cell test, as mapgen-ui.js does.
//
//   node crossport.mjs <overpass.json> <tiles.txt> <outdir>
import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";
import { buildEbm, buildPoi } from "../../../docs/mapgen.js";

const [jsonPath, tilesPath, outDir] = process.argv.slice(2);
const json = JSON.parse(fs.readFileSync(jsonPath, "utf8"));
fs.mkdirSync(outDir, { recursive: true });

let latLngToCell = null;
if (process.env.H3JS_DIR) {
  try {
    latLngToCell = createRequire(path.join(process.env.H3JS_DIR, "x.js"))("h3-js").latLngToCell;
  } catch (e) {
    console.error(`h3-js not loadable from ${process.env.H3JS_DIR}: ${e.message}`);
  }
}

function roadsEnd(b) {
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const nx = dv.getInt32(28, true), ny = dv.getInt32(32, true);
  let end = 36 + nx * ny * 8;
  for (let k = 0; k < nx * ny; k++) {
    const off = dv.getUint32(36 + k * 8, true), len = dv.getUint32(40 + k * 8, true);
    if (off && off + len > end) end = off + len;
  }
  return end;
}

for (const line of fs.readFileSync(tilesPath, "utf8").split("\n")) {
  const f = line.trim().split(/\s+/);
  if (f.length !== 5) continue;
  const [id, s, w, n, e] = [f[0], ...f.slice(1).map(Number)];
  const ebm = buildEbm(json, { s, w, n, e });
  fs.writeFileSync(path.join(outDir, `${id}.ebm`), ebm.subarray(0, roadsEnd(ebm)));
  fs.writeFileSync(path.join(outDir, `${id}.bbox.poi`), buildPoi(json, { s, w, n, e, cell: id }));
  if (latLngToCell) {
    fs.writeFileSync(path.join(outDir, `${id}.poi`), buildPoi(json, {
      s, w, n, e, cell: id, contains: (la, lo) => latLngToCell(la, lo, 6) === id,
    }));
  }
}
