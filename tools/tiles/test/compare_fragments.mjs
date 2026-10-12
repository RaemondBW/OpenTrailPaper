// Border-phase equivalence, step 2: every cell the multi-region run built
// (interior fragments + border fragments) against a single build from the
// extracts merged into one file (the same snapshot, no borders at all).
//
//   node compare_fragments.mjs <multi-region out> <reference out>
import fs from "node:fs";
import path from "node:path";

const [multi, ref] = process.argv.slice(2);
const read = (d) => fs.readdirSync(path.join(d, "v1/regions")).map((f) => JSON.parse(fs.readFileSync(path.join(d, "v1/regions", f), "utf8")));
const refCells = Object.assign({}, ...read(ref).map((f) => f.cells));
let bad = 0;
for (const f of read(multi)) {
  let same = 0, diff = 0, noref = 0;
  for (const [id, r] of Object.entries(f.cells)) {
    const q = refCells[id];
    if (!q) { noref++; continue; }
    if (q[1] === r[1] && q[3] === r[3]) same++;
    else { diff++; console.log(`DIFF ${id} (${f.region} ${f.phase})${q[1] !== r[1] ? " .ebm" : ""}${q[3] !== r[3] ? " .poi" : ""}`); }
  }
  bad += diff + noref;
  console.log(`${f.region} ${f.phase}: ${same} identical, ${diff} different, ${noref} not in the reference`);
}
process.exitCode = bad ? 1 : 0;
