// Equivalence test, step 3: compare the builder's output tree with the app's
// per-cell files. The builder omits a .poi with no POIs (the index says
// poiSize 0 and the apps synthesise it), so for those the reference must be
// exactly the empty file mapgen.buildPoi writes for the cell.
//
//   node compare.mjs <builder-out> <app-out> <tiles.txt>
import fs from "node:fs";
import path from "node:path";
import { buildPoi } from "../../../docs/mapgen.js";
import { tileKey } from "../build_region.mjs";

const [built, app, tilesTxt] = process.argv.slice(2);
let same = 0, diff = 0, missing = 0, poiSame = 0, poiDiff = 0, bytes = 0;
const firstDiff = (a, b) => { const n = Math.min(a.length, b.length); for (let i = 0; i < n; i++) if (a[i] !== b[i]) return i; return n; };
for (const line of fs.readFileSync(tilesTxt, "utf8").trim().split("\n")) {
  const [id, s, w, n, e] = line.split(" ");
  const t = { s: +s, w: +w, n: +n, e: +e };
  const a = path.join(built, tileKey(id, ".ebm")), b = path.join(app, `${id}.ebm`);
  if (!fs.existsSync(a) || !fs.existsSync(b)) { missing++; console.log(`MISSING ${id} builder=${fs.existsSync(a)} app=${fs.existsSync(b)}`); continue; }
  const x = fs.readFileSync(a), y = fs.readFileSync(b);
  bytes += x.length;
  if (Buffer.compare(x, y) === 0) same++;
  else { diff++; console.log(`DIFF ${id}.ebm builder ${x.length} B, app ${y.length} B, first byte ${firstDiff(x, y)}`); }
  const pa = path.join(built, tileKey(id, ".poi"));
  const px = fs.existsSync(pa) ? fs.readFileSync(pa) : Buffer.from(buildPoi({ elements: [] }, { ...t, cell: id }));
  const py = fs.readFileSync(path.join(app, `${id}.poi`));
  if (Buffer.compare(px, py) === 0) poiSame++;
  else { poiDiff++; console.log(`DIFF ${id}.poi builder ${px.length} B, app ${py.length} B, first byte ${firstDiff(px, py)}`); }
}
console.log(`.ebm: ${same} identical, ${diff} different, ${missing} missing (${(bytes / 1048576).toFixed(2)} MB compared); .poi: ${poiSame} identical, ${poiDiff} different`);
process.exitCode = diff || missing || poiDiff ? 1 : 0;
