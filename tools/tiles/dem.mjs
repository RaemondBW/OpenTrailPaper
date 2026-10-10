// Elevation from the Copernicus GLO-90 DEM — the dataset Open-Meteo's
// elevation API serves, which is what the apps sample today (rate-limited:
// ~250 hexes per IP per day, and tiles silently lose ELV1 when it 429s).
//
// Source: the AWS Open Data copy, public HTTPS, no key:
//   https://copernicus-dem-90m.s3.amazonaws.com/<name>/<name>.tif
//   <name> = Copernicus_DSM_COG_30_<N|S>dd_00_<E|W>ddd_00_DEM
// 1°×1° Cloud-Optimised GeoTIFFs, float32 metres, PixelIsPoint: pixel (0,0)
// is centred ON the tile's north-west corner, rows step 3" south, columns
// step 3"×(1, 1.5, 2, 3, 5, 10) east depending on latitude. Tiles over open
// sea do not exist (height 0 there, as Open-Meteo returns).
//
// Licence: "Copernicus DEM — © DLR e.V. 2010-2014 and © Airbus Defence and
// Space GmbH 2014-2018 provided under COPERNICUS by the European Union and
// ESA; all rights reserved" (free for any use with attribution).
import fs from "node:fs";
import path from "node:path";
import { fromArrayBuffer } from "geotiff";

const BASE = "https://copernicus-dem-90m.s3.amazonaws.com";

export function tileName(latFloor, lonFloor) {
  const ns = latFloor >= 0 ? "N" : "S", ew = lonFloor >= 0 ? "E" : "W";
  const la = String(Math.abs(latFloor)).padStart(2, "0"), lo = String(Math.abs(lonFloor)).padStart(3, "0");
  return `Copernicus_DSM_COG_30_${ns}${la}_00_${ew}${lo}_00_DEM`;
}

export class Dem {
  // cacheDir: where downloaded .tif files are kept between runs (optional).
  // mode: "openmeteo" (default) — what api.open-meteo.com/v1/elevation
  //       returns: the post whose cell [row, row+1)×[col, col+1) contains the
  //       point (floor, i.e. the grid read as pixel corners), rounded to whole
  //       metres. Matched Open-Meteo on 100/100 points over hilly San
  //       Francisco (tools/tiles/test/equivalence.sh);
  //       "bilinear" — smoother, but differs from what the apps store today.
  constructor({ cacheDir = null, mode = "openmeteo", maxTiles = 48, userAgent = "OpenTrailPaper tile builder" } = {}) {
    this.cacheDir = cacheDir;
    this.mode = mode;
    this.maxTiles = maxTiles;
    this.ua = userAgent;
    this.tiles = new Map();        // name -> Promise<{w,h,data,lon0,lat0,dx,dy}|null> (LRU by reinsertion)
    this.exists = null;            // Set of tile names from tileList.txt
    this.stats = { downloads: 0, bytes: 0, missing: 0 };
  }

  async #list() {
    if (this.exists) return this.exists;
    const local = this.cacheDir && path.join(this.cacheDir, "tileList.txt");
    let text = null;
    if (local && fs.existsSync(local)) text = fs.readFileSync(local, "utf8");
    else {
      const r = await fetch(`${BASE}/tileList.txt`, { headers: { "User-Agent": this.ua } });
      if (!r.ok) throw new Error(`DEM tile list: HTTP ${r.status}`);
      text = await r.text();
      if (local) { fs.mkdirSync(this.cacheDir, { recursive: true }); fs.writeFileSync(local, text); }
    }
    this.exists = new Set(text.split(/\s+/).filter(Boolean));
    return this.exists;
  }

  async #load(name) {
    const list = await this.#list();
    if (!list.has(name)) { this.stats.missing++; return null; }
    let buf = null;
    const local = this.cacheDir && path.join(this.cacheDir, name + ".tif");
    if (local && fs.existsSync(local)) buf = fs.readFileSync(local);
    else {
      for (let attempt = 0; attempt < 4 && !buf; attempt++) {
        try {
          const r = await fetch(`${BASE}/${name}/${name}.tif`, { headers: { "User-Agent": this.ua } });
          if (r.status === 404 || r.status === 403) { this.stats.missing++; return null; }
          if (!r.ok) throw new Error(`HTTP ${r.status}`);
          buf = Buffer.from(await r.arrayBuffer());
        } catch (e) {
          if (attempt === 3) throw new Error(`DEM ${name}: ${e.message}`);
          await new Promise((res) => setTimeout(res, 1000 * (attempt + 1)));
        }
      }
      this.stats.downloads++; this.stats.bytes += buf.length;
      if (local) { fs.mkdirSync(this.cacheDir, { recursive: true }); fs.writeFileSync(local, buf); }
    }
    const tiff = await fromArrayBuffer(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength));
    const im = await tiff.getImage();
    const [lon0, lat0] = im.getOrigin();
    const [dx, dy] = im.getResolution();
    const data = (await im.readRasters({ interleave: true }));
    return { w: im.getWidth(), h: im.getHeight(), data, lon0, lat0, dx, dy: -dy };
  }

  async #tile(latFloor, lonFloor) {
    const name = tileName(latFloor, ((lonFloor + 180) % 360 + 360) % 360 - 180);
    let p = this.tiles.get(name);
    if (p) { this.tiles.delete(name); this.tiles.set(name, p); return p; }
    p = this.#load(name);
    this.tiles.set(name, p);
    while (this.tiles.size > this.maxTiles) this.tiles.delete(this.tiles.keys().next().value);
    return p;
  }

  // Height of the DEM post at row r, column c of the 1°-tile grid that holds
  // (latFloor, lonFloor); rows/cols past the edge come from the neighbour.
  async #post(t, latFloor, lonFloor, r, c) {
    if (r >= t.h) { latFloor -= 1; r -= t.h; t = await this.#tile(latFloor, lonFloor); if (!t) return 0; }
    if (c >= t.w) { lonFloor += 1; c -= t.w; t = await this.#tile(latFloor, lonFloor); if (!t) return 0; }
    const v = t.data[r * t.w + c];
    return Number.isFinite(v) && v > -1000 ? v : 0;
  }

  // Elevation in metres at (lat, lon); 0 over sea / no data.
  async at(lat, lon) {
    const latFloor = Math.floor(lat), lonFloor = Math.floor(lon);
    // Pixel (0,0) is centred on (latFloor+1, lonFloor), so a point exactly on
    // a tile's southern edge belongs to row 0 of the tile below; handle that
    // via the row overflow in #post.
    const t = await this.#tile(latFloor, lonFloor);
    if (!t) return 0;
    const fr = (latFloor + 1 - lat) / t.dy;          // 0 .. h
    const fc = (lon - lonFloor) / t.dx;              // 0 .. w
    if (this.mode === "openmeteo") return Math.round(await this.#post(t, latFloor, lonFloor, Math.floor(fr), Math.floor(fc)));
    const r0 = Math.floor(fr), c0 = Math.floor(fc);
    const ar = fr - r0, ac = fc - c0;
    const v00 = await this.#post(t, latFloor, lonFloor, r0, c0);
    const v01 = await this.#post(t, latFloor, lonFloor, r0, c0 + 1);
    const v10 = await this.#post(t, latFloor, lonFloor, r0 + 1, c0);
    const v11 = await this.#post(t, latFloor, lonFloor, r0 + 1, c0 + 1);
    return (v00 * (1 - ac) + v01 * ac) * (1 - ar) + (v10 * (1 - ac) + v11 * ac) * ar;
  }
}
