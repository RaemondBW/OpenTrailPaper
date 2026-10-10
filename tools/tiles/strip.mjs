// Border strips: the slice of one extract that cells built in the border
// phase need (regions.mjs). Each region job writes one; the border phase
// loads every strip that reaches its cells and merges them into a single
// in-memory extract with the same shape loadOsm() returns, so the very same
// OverpassEmu + builders run on it.
//
// File: gzip'd NDJSON.
//   {"strip":1,"region":…,"osm":…,"bbox":{s,w,n,e}}         header
//   {"w":[id,version,level,tags,[nodeId…],[lat e7…],[lon e7…]]}
//   {"p":{kind,id,ver,tags,lat,lon}}                       a POI
//
// Merging: the same way or POI can come from several extracts (Geofabrik
// polygons overlap). The highest OSM version wins (the extracts may be a few
// hours apart); a way's bike-route level is the highest any extract saw (a
// route relation is in every extract that holds one of its members, so they
// agree unless the snapshots differ).
import fs from "node:fs";
import zlib from "node:zlib";
import readline from "node:readline";
import { once } from "node:events";
import { mapWayMatch, poiWayMatch, K_MAP, K_COAST, K_POI, MISSING } from "./osmstore.mjs";

export async function writeStrip(file, { store, wayIdx, pois, region, osm }) {
  const { ways, refs, tags } = store;
  const gz = zlib.createGzip({ level: 6 });
  const out = fs.createWriteStream(file);
  gz.pipe(out);
  const write = async (obj) => { if (!gz.write(JSON.stringify(obj) + "\n")) await once(gz, "drain"); };
  let s = 90, w = 180, n = -90, e = -180;
  const idx = [...wayIdx].sort((a, b) => ways.id[a] - ways.id[b]);
  for (const i of idx) {
    for (let k = ways.start[i]; k < ways.start[i] + ways.len[i]; k++) {
      if (refs.lat[k] === MISSING) continue;
      const la = refs.lat[k] / 1e7, lo = refs.lon[k] / 1e7;
      if (la < s) s = la; if (la > n) n = la; if (lo < w) w = lo; if (lo > e) e = lo;
    }
  }
  for (const p of pois) { s = Math.min(s, p.lat); n = Math.max(n, p.lat); w = Math.min(w, p.lon); e = Math.max(e, p.lon); }
  await write({ strip: 1, region, osm, bbox: { s, w, n, e }, ways: idx.length, pois: pois.length });
  for (const i of idx) {
    const a = ways.start[i], b = a + ways.len[i];
    await write({ w: [ways.id[i], ways.ver[i], ways.level[i], tags[ways.tag[i]],
      Array.from(refs.id.subarray(a, b)), Array.from(refs.lat.subarray(a, b)), Array.from(refs.lon.subarray(a, b))] });
  }
  for (const p of pois) await write({ p });
  gz.end();
  await once(out, "close");
  return { ways: idx.length, pois: pois.length, bbox: { s, w, n, e } };
}

export async function readStripHeader(file) {
  const rl = readline.createInterface({ input: fs.createReadStream(file).pipe(zlib.createGunzip()), crlfDelay: Infinity });
  for await (const line of rl) { rl.close(); return JSON.parse(line); }
  return null;
}

// Merge strips (in the given order: earlier wins a version tie) into a
// loadOsm()-shaped store.
export async function loadStrips(files, log = () => {}) {
  const ways = new Map();   // id -> [ver, level, tags, nids, lats, lons]
  const pois = new Map();   // kind:id -> poi
  const headers = [];
  for (const f of files) {
    const rl = readline.createInterface({ input: fs.createReadStream(f).pipe(zlib.createGunzip()), crlfDelay: Infinity });
    let first = true;
    for await (const line of rl) {
      const o = JSON.parse(line);
      if (first) { headers.push(o); first = false; continue; }
      if (o.w) {
        const [id, ver, level, tags, nids, lats, lons] = o.w;
        const cur = ways.get(id);
        if (!cur) ways.set(id, [ver, level, tags, nids, lats, lons]);
        else {
          const lvl = Math.max(cur[1], level);
          if (ver > cur[0]) ways.set(id, [ver, lvl, tags, nids, lats, lons]);
          else cur[1] = lvl;
        }
      } else if (o.p) {
        const k = o.p.kind + o.p.id;
        const cur = pois.get(k);
        if (!cur || o.p.ver > cur.ver) pois.set(k, o.p);
      }
    }
  }
  const ids = [...ways.keys()].sort((a, b) => a - b);
  let nRefs = 0;
  for (const id of ids) nRefs += ways.get(id)[3].length;
  const W = {
    id: new Float64Array(ids.length), start: new Uint32Array(ids.length), len: new Uint32Array(ids.length),
    tag: new Uint32Array(ids.length), kind: new Uint8Array(ids.length), ver: new Uint32Array(ids.length),
    level: new Uint8Array(ids.length),
  };
  const R = { id: new Float64Array(nRefs), lat: new Int32Array(nRefs), lon: new Int32Array(nRefs) };
  const tagList = [], tagIdx = new Map();
  const intern = (t) => {
    const key = Object.keys(t).sort().map((k) => k + "\u0001" + t[k]).join("\u0002");
    let i = tagIdx.get(key);
    if (i === undefined) { i = tagList.length; tagList.push(Object.freeze(t)); tagIdx.set(key, i); }
    return i;
  };
  let r = 0;
  ids.forEach((id, i) => {
    const [ver, level, tags, nids, lats, lons] = ways.get(id);
    W.id[i] = id; W.ver[i] = ver; W.level[i] = level; W.tag[i] = intern(tags);
    W.kind[i] = (mapWayMatch(tags) ? K_MAP : 0) | (tags.natural === "coastline" ? K_COAST : 0) | (poiWayMatch(tags) ? K_POI : 0);
    W.start[i] = r; W.len[i] = nids.length;
    for (let k = 0; k < nids.length; k++, r++) { R.id[r] = nids[k]; R.lat[r] = lats[k]; R.lon[r] = lons[k]; }
  });
  const order = { n: 0, w: 1, r: 2 };
  const P = [...pois.values()].sort((a, b) => order[a.kind] - order[b.kind] || a.id - b.id);
  log(`strips: ${files.length} files, ${ids.length} ways, ${nRefs} refs, ${P.length} POIs`);
  return { store: { ways: W, refs: R, tags: tagList, pois: P, sorted: true }, headers };
}
