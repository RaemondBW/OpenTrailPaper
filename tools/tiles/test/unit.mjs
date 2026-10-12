// Unit tests for the tile builder's own logic (no network, no osmium).
//   npm test --prefix tools/tiles
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { assembleCoastline } from "../../../docs/mapgen.js";
import { assembleCoastlineFast } from "../coastline.mjs";
import { Region, bboxGeometry } from "../poly.mjs";
import { merge } from "../merge_index.mjs";
import { resume } from "../resume.mjs";
import { writeStrip, loadStrips, boxFilter } from "../strip.mjs";
import { swiftRound, clipToBox } from "../apptile.mjs";

let n = 0;
const test = async (name, fn) => { await fn(); n++; console.log(`ok - ${name}`); };

// Deterministic PRNG.
let seed = 12345;
const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);

await test("coastline join matches mapgen.assembleCoastline", () => {
  for (let trial = 0; trial < 300; trial++) {
    // Random chains over a small node pool so endpoints collide often,
    // including reversed joins, loops and duplicate ways.
    const pool = 4 + Math.floor(rnd() * 12);
    const nodes = new Map();
    for (let i = 1; i <= pool + 40; i++) nodes.set(i, [rnd(), rnd()]);
    const ways = [];
    const nw = 1 + Math.floor(rnd() * 14);
    for (let w = 0; w < nw; w++) {
      const len = 2 + Math.floor(rnd() * 4);
      const way = [];
      for (let k = 0; k < len; k++) way.push(k === 0 || k === len - 1 ? 1 + Math.floor(rnd() * pool) : pool + 1 + Math.floor(rnd() * 40));
      if (rnd() < 0.05) way.push(9999);   // a node missing from the table
      ways.push(way);
    }
    const a = assembleCoastline(ways, nodes), b = assembleCoastlineFast(ways, nodes);
    assert.deepEqual(b, a, `trial ${trial}: ${JSON.stringify(ways)}`);
  }
});

await test("swiftRound rounds half away from zero", () => {
  assert.equal(swiftRound(2.5), 3); assert.equal(swiftRound(-2.5), -3);
  assert.equal(swiftRound(0.49999), 0); assert.equal(swiftRound(-0.5), -1);
});

await test("clipToBox keeps an inner polygon and clips an outer one", () => {
  const inner = [[0.2, 0.2], [0.2, 0.8], [0.8, 0.8], [0.8, 0.2]];
  assert.deepEqual(clipToBox(inner, 0, 0, 1, 1), inner);
  const big = [[-1, -1], [-1, 2], [2, 2], [2, -1]];
  const c = clipToBox(big, 0, 0, 1, 1);
  assert.equal(c.length, 4);
  for (const [la, lo] of c) assert.ok(la >= 0 && la <= 1 && lo >= 0 && lo <= 1);
});

await test("Region point / rectangle tests", () => {
  // An L-shaped region: [0,2]x[0,1] plus [0,1]x[1,2] (lat, lon).
  const L = new Region({ type: "Polygon", coordinates: [[[0, 0], [1, 0], [1, 2], [2, 2], [2, 0], [0, 0]].map(([lo, la]) => [lo, la])] });
  const sq = new Region(bboxGeometry(0, 0, 1, 1));
  assert.ok(sq.containsPoint(0.5, 0.5));
  assert.ok(!sq.containsPoint(1.5, 0.5));
  assert.ok(sq.containsRect(0.1, 0.1, 0.9, 0.9));
  assert.ok(!sq.containsRect(0.1, 0.1, 1.1, 0.9));
  assert.ok(sq.intersectsRect(0.9, 0.9, 2, 2));
  assert.ok(!sq.intersectsRect(1.1, 1.1, 2, 2));
  assert.ok(sq.intersectsRect(-1, -1, 2, 2));        // rectangle around the region
  assert.ok(L.bbox.n >= 2);
});

const frag = (region, phase, cells) => ({ version: "v1", region, phase, cells });

await test("merge: indexes, deletions, failed and dropped regions", () => {
  const A = "862a33007ffffff", B = "862a3300fffffff", C = "861f8222fffffff", D = "861f82277ffffff";
  const oldFrags = new Map([
    ["a", frag("a", "interior", { [A]: [10, "h1", 0, ""], [B]: [20, "h2", 5, "p2"] })],
    ["a.border", frag("a", "border", { [C]: [30, "h3", 0, ""] })],
    ["gone", frag("gone", "interior", { [D]: [40, "h4", 0, ""] })],
  ]);
  // This run: a rebuilt (B lost its POIs, A unchanged); a.border's job
  // failed; region "gone" is no longer in the plan.
  const newFrags = new Map([["a", frag("a", "interior", { [A]: [10, "h1", 0, ""], [B]: [21, "h2b", 0, ""] })]]);
  const r = merge({ newFrags, oldFrags, order: ["a", "b"] });
  assert.equal(r.cells, 3);                                   // A, B and C (kept)
  assert.ok(r.now.has("a.border") && !r.now.has("gone"));
  assert.deepEqual([...r.indexes.keys()].sort(), ["861f82", "862a33"]);  // 861f82 lost D
  assert.ok(r.del.includes("v1/862a33/862a3300fffffff.poi"));
  assert.ok(r.del.includes("v1/861f82/861f82277ffffff.ebm"));
  assert.ok(!r.del.some((k) => k.includes(A) || k.includes(C)));
  const idx = JSON.parse(r.indexes.get("862a33"));
  assert.deepEqual(idx, { v: 1, cells: { [A]: [10, "h1", 0, ""], [B]: [21, "h2b", 0, ""] }, regions: ["a"] });
});

await test("merge: a partial (--only) run keeps every region it did not build", () => {
  const M = "861eb4d97ffffff", K = "862a33007ffffff";
  const oldFrags = new Map([["monaco.border", frag("monaco", "border", { [M]: [1084, "hm", 0, ""] })]]);
  const newFrags = new Map([["us_california", frag("us/california", "interior", { [K]: [9, "hk", 0, ""] })]]);
  const r = merge({ newFrags, oldFrags, order: ["us/california"], partial: true });
  assert.ok(r.now.has("monaco.border") && r.now.has("us_california"));
  assert.equal(r.cells, 2);
  assert.ok(!r.del.some((k) => k.includes(M) || k.includes("861eb4")));
  // The same run as a full plan would drop Monaco (no longer in Geofabrik's tree).
  const full = merge({ newFrags, oldFrags, order: ["us/california"] });
  assert.ok(!full.now.has("monaco.border") && full.del.some((k) => k.includes(M)));
});

await test("resume: redo failed jobs and the borders that read them", () => {
  const plan = { regions: [{ id: "a" }, { id: "antarctica" }, { id: "c" }, { id: "d" }], partial: false, jobs: [
    { name: "j01", regions: ["a", "antarctica"], needs: ["j01", "j02"] },
    { name: "j02", regions: ["c"], needs: ["j02"] },
    { name: "j03", regions: ["d"], needs: ["j03"] },
  ] };
  const results = new Map([["interior (j01)", "failure"], ["interior (j02)", "success"], ["interior (j03)", "success"],
    ["border (j01)", "failure"], ["border (j02)", "success"], ["border (j03)", "failure"]]);
  const r = resume(plan, [{ id: 7, results }]);
  assert.deepEqual(r.interior, ["j01"]);
  assert.deepEqual(r.border, ["j01", "j03"]);
  assert.equal(r.plan.partial, true);
  assert.deepEqual(r.plan.jobs[0].regions, ["a"]);
  assert.ok(!r.plan.regions.some((x) => x.id === "antarctica"));
});

await test("resume: a resume of a resume reaches back for strips", () => {
  const plan = { regions: [{ id: "a" }, { id: "c" }, { id: "d" }], partial: true, jobs: [
    { name: "j01", regions: ["a"], needs: ["j01", "j02"] },
    { name: "j02", regions: ["c"], needs: ["j02", "j03"] },
    { name: "j03", regions: ["d"], needs: ["j03"] },
  ] };
  const first = new Map([["interior (j01)", "failure"], ["interior (j02)", "success"], ["interior (j03)", "success"],
    ["border (j01)", "failure"], ["border (j02)", "success"], ["border (j03)", "failure"]]);
  // The resume rebuilt j01 and its border; j03's border failed again.
  const second = new Map([["interior (j01)", "success"], ["border (j01)", "success"], ["border (j03)", "failure"]]);
  const r = resume(plan, [{ id: 2, results: second }, { id: 1, results: first }]);
  assert.deepEqual(r.interior, []);
  assert.deepEqual(r.border, ["j03"]);
  assert.deepEqual(r.plan.stripsFrom, { j01: "2", j02: "1", j03: "1" });
});

await test("merge: a group with no cells left loses its index", () => {
  const X = "862a33007ffffff";
  const oldFrags = new Map([["a", frag("a", "interior", { [X]: [1, "h", 0, ""] })]]);
  const r = merge({ newFrags: new Map([["a", frag("a", "interior", {})]]), oldFrags, order: ["a"] });
  assert.ok(r.del.includes("v1/862a33/index.json"));
  assert.ok(r.del.includes("v1/862a33/862a33007ffffff.ebm"));
});

await test("strips: round trip, version wins, levels merge, box filter", async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "otp-strip-"));
  const mk = (ways, pois) => {
    let refs = 0; for (const w of ways) refs += w.nodes.length;
    const W = { id: new Float64Array(ways.length), start: new Uint32Array(ways.length), len: new Uint32Array(ways.length),
      tag: new Uint32Array(ways.length), kind: new Uint8Array(ways.length), ver: new Uint32Array(ways.length), level: new Uint8Array(ways.length) };
    const R = { id: new Float64Array(refs), lat: new Int32Array(refs), lon: new Int32Array(refs) };
    const tags = [];
    let r = 0;
    ways.forEach((w, i) => {
      W.id[i] = w.id; W.ver[i] = w.ver; W.level[i] = w.level; W.tag[i] = tags.push(w.tags) - 1;
      W.start[i] = r; W.len[i] = w.nodes.length;
      for (const [nid, la, lo] of w.nodes) { R.id[r] = nid; R.lat[r] = la * 1e7; R.lon[r] = lo * 1e7; r++; }
    });
    return { store: { ways: W, refs: R, tags, pois, sorted: true }, wayIdx: new Set(ways.map((_, i) => i)), pois };
  };
  const road = { highway: "residential" };
  const s1 = mk([
    { id: 5, ver: 2, level: 0, tags: road, nodes: [[1, 1, 1], [2, 1.001, 1.001]] },
    { id: 7, ver: 1, level: 3, tags: road, nodes: [[3, 1, 1], [4, 1.002, 1]] },
    { id: 9, ver: 1, level: 0, tags: road, nodes: [[5, 50, 50], [6, 50.1, 50]] },   // far away
  ], [{ kind: "n", id: 11, ver: 1, tags: { amenity: "toilets" }, lat: 1.0005, lon: 1.0005 }]);
  const s2 = mk([
    { id: 5, ver: 3, level: 1, tags: { highway: "cycleway" }, nodes: [[1, 1, 1], [2, 1.001, 1.002]] },
    { id: 7, ver: 1, level: 0, tags: road, nodes: [[3, 1, 1], [4, 1.002, 1]] },
  ], [{ kind: "n", id: 11, ver: 2, tags: { amenity: "drinking_water" }, lat: 1.0005, lon: 1.0005 }]);
  await writeStrip(path.join(dir, "a.gz"), { ...s1, region: "a", osm: "t" });
  await writeStrip(path.join(dir, "b.gz"), { ...s2, region: "b", osm: "t" });
  const { store } = await loadStrips([path.join(dir, "a.gz"), path.join(dir, "b.gz")], () => {},
    boxFilter([{ s: 0.9, w: 0.9, n: 1.1, e: 1.1 }]));
  assert.deepEqual(Array.from(store.ways.id), [5, 7]);           // 9 filtered out
  assert.equal(store.tags[store.ways.tag[0]].highway, "cycleway"); // version 3 won
  assert.deepEqual(Array.from(store.ways.level), [1, 3]);        // max level kept
  assert.equal(store.refs.lon[1], 1.002e7);
  assert.equal(store.pois.length, 1);
  assert.equal(store.pois[0].tags.amenity, "drinking_water");
  fs.rmSync(dir, { recursive: true, force: true });
});

console.log(`${n} tests passed`);
