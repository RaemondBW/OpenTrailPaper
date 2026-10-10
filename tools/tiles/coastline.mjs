// A fast, output-identical replacement for mapgen.js assembleCoastline.
//
// assembleCoastline joins coastline ways with a "restart from the top after
// every merge" double loop: each step merges the lexicographically first pair
// (i, j) whose endpoints meet, into chain i. That is O(n^3) and a 0.7° coastal
// box (the apps' padded coastline fetch) can hold thousands of ways — fine
// once on a phone, far too slow for millions of cells.
//
// Same result, faster: once chain i has no partner, no earlier chain can gain
// one later (a merge only ever removes endpoints from the pool or moves them
// onto chain i itself), and a partner j is always > i. So walk i upwards and,
// for each, keep merging with its SMALLEST partner j, using an endpoint index,
// applying the four join rules in mapgen's order. tools/tiles/test/unit.mjs
// checks this against mapgen.assembleCoastline on real and random input.

// coastWays: arrays of node ids. Returns chains of node ids (null-free).
export function joinChains(coastWays) {
  const chains = coastWays.map((nids) => nids.slice());
  const ends = new Map();   // node id -> Set of chain indices with it as an endpoint
  const add = (id, i) => { let s = ends.get(id); if (!s) ends.set(id, (s = new Set())); s.add(i); };
  const del = (id, i) => { const s = ends.get(id); if (s) { s.delete(i); if (!s.size) ends.delete(id); } };
  const reg = (i) => { const c = chains[i]; add(c[0], i); add(c[c.length - 1], i); };
  const unreg = (i) => { const c = chains[i]; del(c[0], i); del(c[c.length - 1], i); };
  for (let i = 0; i < chains.length; i++) if (chains[i].length) reg(i);

  for (let i = 0; i < chains.length; i++) {
    if (chains[i] == null || chains[i].length === 0) continue;
    for (;;) {
      const a = chains[i];
      let j = -1;
      for (const id of [a[0], a[a.length - 1]]) {
        const s = ends.get(id);
        if (!s) continue;
        for (const k of s) if (k !== i && chains[k] != null && (j < 0 || k < j)) j = k;
      }
      if (j < 0) break;
      const b = chains[j];
      let merged;
      if (a[a.length - 1] === b[0]) merged = a.concat(b.slice(1));
      else if (a[a.length - 1] === b[b.length - 1]) merged = a.concat(b.slice(0, -1).reverse());
      else if (a[0] === b[b.length - 1]) merged = b.concat(a.slice(1));
      else merged = b.slice().reverse().concat(a.slice(1));   // a[0] === b[0]
      unreg(i); unreg(j);
      chains[i] = merged; chains[j] = null;
      reg(i);
    }
  }
  return chains.filter((c) => c != null);
}

// Drop-in for mapgen.assembleCoastline(coastWays, nodes).
export function assembleCoastlineFast(coastWays, nodes) {
  const out = [];
  for (const c of joinChains(coastWays)) {
    const pts = [];
    for (const id of c) { const p = nodes.get(id); if (p) pts.push(p); }
    if (pts.length >= 2) out.push(pts);
  }
  return out;
}
