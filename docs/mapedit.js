// Map style editor: pick an area (same Leaflet box picker as the docs site),
// fetch OSM features, and preview them rendered device-style at each zoom level.
// Road tiers + water are built in; extra feature layers (parks, rivers, rail)
// can be added and configured per zoom.
import { classify, assembleCoastline, regionSeaPolygons, OVERPASS_ENDPOINTS } from "./mapgen.js";

const ZOOMS = [1, 2, 4, 8, 16, 32];   // m/px, the device's zoom stops
const TIERS = [
  { cls: 0, key: "arterial",  label: "Arterial",  hint: "motorway · trunk" },
  { cls: 1, key: "secondary", label: "Secondary", hint: "primary · secondary · tertiary" },
  { cls: 2, key: "minor",     label: "Minor",     hint: "residential · unclassified · …" },
  { cls: 3, key: "path",      label: "Path",      hint: "footway · cycleway · track · steps" },
];
// Optional feature layers you can add. `match` assigns a fetched way to the
// layer; `filter` builds its Overpass clause (b = "S,W,N,E"). Drawn in the
// order listed here, under the roads.
const ADDABLE = {
  parks: {
    label: "Parks / green", hint: "leisure=park · landuse=forest/grass · natural=wood",
    kind: "fill", fill: "#d7e7cf",
    match: (t) => t.leisure === "park" || /^(grass|forest|meadow|recreation_ground|cemetery|village_green)$/.test(t.landuse || "") || /^(wood|scrub|grassland|heath)$/.test(t.natural || ""),
    filter: (b) => [`way["leisure"="park"](${b});`,
                    `way["landuse"~"^(grass|forest|meadow|recreation_ground|cemetery|village_green)$"](${b});`,
                    `way["natural"~"^(wood|scrub|grassland|heath)$"](${b});`],
  },
  rivers: {
    label: "Rivers / streams", hint: "waterway=river/stream/canal",
    kind: "line", stroke: "#8fabc4", width: 2,
    match: (t) => /^(river|stream|canal)$/.test(t.waterway || ""),
    filter: (b) => [`way["waterway"~"^(river|stream|canal)$"](${b});`],
  },
  rail: {
    label: "Railways", hint: "railway=rail/subway/tram",
    kind: "line", stroke: "#8a8a8a", width: 2, dash: [6, 4],
    match: (t) => /^(rail|subway|tram|light_rail|narrow_gauge)$/.test(t.railway || ""),
    filter: (b) => [`way["railway"~"^(rail|subway|tram|light_rail|narrow_gauge)$"](${b});`],
  },
};

const DEV_W = 540, DEV_H = 770, SC = 0.7;   // device map area, scaled to fit

let cfg = defaultConfig();
let roads = [], waterPolys = [], layerData = {}, added = [];
let center = null, curMpp = 8, lastBbox = null;

// Defaults mirror the firmware: path shed ≥4, minor ≥8, secondary ≥16, arterial
// never (thinning 5→2 at ≥16). Added layers default to showing in ≤8 m/px.
function defaultConfig() {
  const c = { tiers: {}, water: {}, layers: {} };
  for (const t of TIERS) c.tiers[t.key] = {};
  for (const m of ZOOMS) {
    c.tiers.arterial[m]  = { show: true,   width: m >= 16 ? 2 : 5 };
    c.tiers.secondary[m] = { show: m < 16, width: 3 };
    c.tiers.minor[m]     = { show: m < 8,  width: 2 };
    c.tiers.path[m]      = { show: m < 4,  width: 1 };
    c.water[m] = true;
  }
  return c;
}
function defaultLayerShow() {
  const o = {};
  for (const m of ZOOMS) o[m] = m <= 8;
  return o;
}

const $ = (id) => document.getElementById(id);
const cv = $("cv"), ctx = cv.getContext("2d");
const setStatus = (m) => { $("status").textContent = m; };

// --- Overpass (custom query so added layers come down too) -----------------
async function overpass(filters, onStatus) {
  const q = `[out:json][timeout:90];\n(\n${filters.join("\n")}\n);\nout body;\n>;\nout skel qt;`;
  const body = "data=" + encodeURIComponent(q);
  const total = OVERPASS_ENDPOINTS.length * 2;
  let lastErr = null;
  for (let i = 0; i < total; i++) {
    const url = OVERPASS_ENDPOINTS[i % OVERPASS_ENDPOINTS.length];
    onStatus(`server ${new URL(url).host} (try ${i + 1}/${total})…`);
    try {
      const r = await fetch(url, {
        method: "POST", body,
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
      });
      if (r.ok) return await r.json();
      lastErr = new Error("HTTP " + r.status);
    } catch (e) { lastErr = e; }
  }
  throw lastErr || new Error("Overpass unreachable");
}

// --- area picker (same Leaflet box selector as the docs site) --------------
const bbN = $("bb-n"), bbS = $("bb-s"), bbW = $("bb-w"), bbE = $("bb-e");
const round5 = (v) => Math.round(v * 1e5) / 1e5;

function readBbox() {
  const s = parseFloat(bbS.value), w = parseFloat(bbW.value);
  const n = parseFloat(bbN.value), e = parseFloat(bbE.value);
  if ([s, w, n, e].some((v) => !isFinite(v)) || s >= n || w >= e) return null;
  return { s, w, n, e };
}
function setBbox(s, w, n, e) {
  bbS.value = round5(s); bbW.value = round5(w);
  bbN.value = round5(n); bbE.value = round5(e);
  drawRect();
}

let lmap = null, rect = null, drawing = false, dragStart = null;
if (typeof L !== "undefined") {
  lmap = L.map("pickmap", { zoomControl: true }).setView([37.7625, -122.44], 12);
  L.tileLayer("https://tile.openstreetmap.org/{z}/{x}/{y}.png",
    { maxZoom: 19, attribution: "&copy; OpenStreetMap contributors" }).addTo(lmap);
  setTimeout(() => lmap.invalidateSize(), 200);

  $("useview").onclick = () => {
    const b = lmap.getBounds();
    setBbox(b.getSouth(), b.getWest(), b.getNorth(), b.getEast());
  };
  $("drawbox").onclick = () => setDrawing(!drawing);
  lmap.on("mousedown", (e) => {
    if (!drawing) return;
    dragStart = e.latlng; L.DomEvent.preventDefault(e.originalEvent);
  });
  lmap.on("mousemove", (e) => { if (drawing && dragStart) drawRectBounds(L.latLngBounds(dragStart, e.latlng)); });
  lmap.on("mouseup", (e) => {
    if (!drawing || !dragStart) return;
    const b = L.latLngBounds(dragStart, e.latlng);
    dragStart = null; setDrawing(false);
    setBbox(b.getSouth(), b.getWest(), b.getNorth(), b.getEast());
  });
} else {
  ["pickmap", "drawbox", "useview"].forEach((id) => { const el = $(id); if (el) el.style.display = "none"; });
}

function setDrawing(on) {
  drawing = on;
  $("drawbox").classList.toggle("active", on);
  if (!lmap) return;
  lmap.getContainer().style.cursor = on ? "crosshair" : "";
  on ? lmap.dragging.disable() : lmap.dragging.enable();
}
function drawRectBounds(b) {
  if (!lmap) return;
  if (!rect) rect = L.rectangle(b, { color: "#F4501E", weight: 2, fillOpacity: 0.08 }).addTo(lmap);
  else rect.setBounds(b);
}
function drawRect() {
  const bb = readBbox();
  if (bb && lmap) drawRectBounds(L.latLngBounds([bb.s, bb.w], [bb.n, bb.e]));
}
[bbN, bbS, bbW, bbE].forEach((f) => f.addEventListener("input", drawRect));

$("load").onclick = () => loadArea(readBbox());
$("reset").onclick = () => { cfg = defaultConfig(); for (const k of added) cfg.layers[k] = defaultLayerShow(); buildMatrix(); render(); };
$("export").onclick = () => { const o = $("out"); o.hidden = false; o.textContent = JSON.stringify({ ...cfg, added }, null, 2); };

const HIGHWAYS = `way["highway"~"^(motorway|trunk|primary|secondary|tertiary|residential|unclassified|living_street|pedestrian|cycleway|footway|path|track|steps)"]`;

async function loadArea(bb) {
  if (!bb) { setStatus("Draw or enter a valid box (north above south, east right of west)."); return; }
  const midLat = (bb.s + bb.n) / 2;
  const kmW = (bb.e - bb.w) * 111.32 * Math.cos(midLat * Math.PI / 180);
  const kmH = (bb.n - bb.s) * 110.57;
  if (kmW * kmH > 40000) { setStatus("Too large — draw a smaller box (Overpass will refuse it)."); return; }
  const b = `${bb.s},${bb.w},${bb.n},${bb.e}`;
  const filters = [`${HIGHWAYS}(${b});`, `way["natural"="water"](${b});`, `way["natural"="coastline"](${b});`];
  for (const key of added) for (const f of ADDABLE[key].filter(b)) filters.push(f);

  $("load").disabled = true;
  setStatus("Downloading OSM data…");
  try {
    const json = await overpass(filters, setStatus);
    parse(json, bb);
    lastBbox = bb;
    center = [midLat, (bb.w + bb.e) / 2];
    const extra = added.length ? " · " + added.map((k) => `${layerData[k].length} ${k}`).join(", ") : "";
    setStatus(`Loaded ${roads.length} roads, ${waterPolys.length} water${extra} · ${kmW.toFixed(1)}×${kmH.toFixed(1)} km.`);
    render();
  } catch (e) {
    setStatus("Failed: " + (e && e.message ? e.message : e));
  }
  $("load").disabled = false;
}

function parse(json, bb) {
  const nodes = new Map();
  for (const el of json.elements) if (el.type === "node") nodes.set(el.id, [el.lat, el.lon]);
  roads = []; waterPolys = [];
  layerData = {}; for (const k of added) layerData[k] = [];
  const coastWays = [];
  for (const el of json.elements) {
    if (el.type !== "way" || !el.nodes) continue;
    const tags = el.tags || {};
    if (tags.natural === "coastline") { coastWays.push(el.nodes); continue; }
    const pts = el.nodes.map((id) => nodes.get(id)).filter(Boolean);
    if (pts.length < 2) continue;
    if (tags.natural === "water") { waterPolys.push(pts); continue; }
    const cls = classify(tags);
    if (cls != null) { roads.push({ cls, pts }); continue; }
    for (const k of added) if (ADDABLE[k].match(tags)) { layerData[k].push(pts); break; }
  }
  if (coastWays.length) {
    const chains = assembleCoastline(coastWays, nodes);
    for (const ring of regionSeaPolygons(chains, bb.s, bb.w, bb.n, bb.e)) waterPolys.push(ring);
  }
}

function render() {
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.fillStyle = "#fff";
  ctx.fillRect(0, 0, cv.width, cv.height);
  $("pvinfo").textContent = center
    ? `${curMpp} m/px · view ≈ ${(DEV_W * curMpp / 1000).toFixed(1)}×${(DEV_H * curMpp / 1000).toFixed(1)} km`
    : "— m/px · load an area to render";
  if (!center) return;

  const [clat, clon] = center;
  const kx = 111320 * Math.cos(clat * Math.PI / 180), ky = 110540;
  const proj = (p) => [
    (DEV_W / 2 + (p[1] - clon) * kx / curMpp) * SC,
    (DEV_H / 2 - (p[0] - clat) * ky / curMpp) * SC,
  ];
  const trace = (way) => { way.forEach((p, i) => { const q = proj(p); i ? ctx.lineTo(q[0], q[1]) : ctx.moveTo(q[0], q[1]); }); };

  const fillLayer = (ways, color) => {
    ctx.fillStyle = color;
    for (const w of ways) { ctx.beginPath(); trace(w); ctx.closePath(); ctx.fill(); }
  };
  const lineLayer = (ways, color, width, dash) => {
    ctx.strokeStyle = color; ctx.lineWidth = Math.max(0.5, width * SC);
    ctx.lineJoin = "round"; ctx.lineCap = "round";
    ctx.setLineDash(dash ? dash.map((d) => d * SC) : []);
    ctx.beginPath(); for (const w of ways) trace(w); ctx.stroke();
    ctx.setLineDash([]);
  };

  // parks → water → added lines → roads (bottom to top)
  if (added.includes("parks") && cfg.layers.parks[curMpp]) fillLayer(layerData.parks, ADDABLE.parks.fill);
  if (cfg.water[curMpp]) fillLayer(waterPolys, "#cfd6dd");
  for (const k of ["rivers", "rail"])
    if (added.includes(k) && cfg.layers[k][curMpp]) lineLayer(layerData[k], ADDABLE[k].stroke, ADDABLE[k].width, ADDABLE[k].dash);

  for (const t of [...TIERS].reverse()) {
    const c = cfg.tiers[t.key][curMpp];
    if (!c.show) continue;
    ctx.strokeStyle = t.cls === 3 ? "#b0b0b0" : "#111";
    ctx.lineWidth = Math.max(0.5, c.width * SC);
    ctx.lineJoin = "round"; ctx.lineCap = "round";
    ctx.beginPath();
    for (const road of roads) if (road.cls === t.cls) trace(road.pts);
    ctx.stroke();
  }
}

function buildMatrix() {
  let html = "<tr><th class='tier'>Layer</th>";
  for (const m of ZOOMS) html += `<th><span class="${m === curMpp ? "cur" : ""}">${m}</span></th>`;
  html += "</tr>";
  for (const t of TIERS) {
    html += `<tr><td class="tier">${t.label}<small>${t.hint}</small></td>`;
    for (const m of ZOOMS) {
      const c = cfg.tiers[t.key][m];
      html += `<td><div class="cell">
        <input type="checkbox" data-t="${t.key}" data-m="${m}" ${c.show ? "checked" : ""}>
        <input class="w" type="number" min="1" max="12" data-tw="${t.key}" data-m="${m}" value="${c.width}">
      </div></td>`;
    }
    html += "</tr>";
  }
  html += `<tr><td class="tier">Water<small>ocean · bay · lakes</small></td>`;
  for (const m of ZOOMS)
    html += `<td><input type="checkbox" data-water="${m}" ${cfg.water[m] ? "checked" : ""}></td>`;
  html += "</tr>";
  for (const k of added) {
    const L = ADDABLE[k];
    html += `<tr><td class="tier">${L.label} <button class="rm" data-rm="${k}" title="Remove layer">✕</button><small>${L.hint}</small></td>`;
    for (const m of ZOOMS)
      html += `<td><input type="checkbox" data-layer="${k}" data-m="${m}" ${cfg.layers[k][m] ? "checked" : ""}></td>`;
    html += "</tr>";
  }
  $("matrix").innerHTML = html;

  const M = $("matrix");
  M.querySelectorAll("input[type=checkbox][data-t]").forEach((el) =>
    (el.onchange = () => { cfg.tiers[el.dataset.t][+el.dataset.m].show = el.checked; render(); }));
  M.querySelectorAll("input[data-tw]").forEach((el) =>
    (el.onchange = () => { cfg.tiers[el.dataset.tw][+el.dataset.m].width = Math.max(1, +el.value || 1); render(); }));
  M.querySelectorAll("input[data-water]").forEach((el) =>
    (el.onchange = () => { cfg.water[+el.dataset.water] = el.checked; render(); }));
  M.querySelectorAll("input[data-layer]").forEach((el) =>
    (el.onchange = () => { cfg.layers[el.dataset.layer][+el.dataset.m] = el.checked; render(); }));
  M.querySelectorAll("button[data-rm]").forEach((el) =>
    (el.onclick = () => removeLayer(el.dataset.rm)));
}

// --- add / remove layers ---------------------------------------------------
function addLayer(key) {
  if (!key || added.includes(key)) return;
  added.push(key);
  cfg.layers[key] = defaultLayerShow();
  buildMatrix(); buildAddBar();
  if (lastBbox) { setStatus(`Fetching “${ADDABLE[key].label}”…`); loadArea(lastBbox); }
  else render();
}
function removeLayer(key) {
  added = added.filter((k) => k !== key);
  delete layerData[key]; delete cfg.layers[key];
  buildMatrix(); buildAddBar(); render();
}
function buildAddBar() {
  const bar = $("addbar");
  const rest = Object.keys(ADDABLE).filter((k) => !added.includes(k));
  if (!rest.length) { bar.innerHTML = "<span class='legend'>All layers added.</span>"; return; }
  let html = `<select id="addsel" style="background:#0f1214;color:#e7ecf0;border:1px solid #2c333a;border-radius:7px;padding:7px 8px;font-size:13px;">`;
  for (const k of rest) html += `<option value="${k}">${ADDABLE[k].label}</option>`;
  html += `</select> <button class="ghost" id="addbtn" type="button">+ Add layer</button>`;
  bar.innerHTML = html;
  $("addbtn").onclick = () => addLayer($("addsel").value);
}

function buildZoombar() {
  const bar = $("zoombar");
  bar.innerHTML = "";
  for (const m of ZOOMS) {
    const btn = document.createElement("button");
    btn.textContent = m + " m/px";
    if (m === curMpp) btn.className = "active";
    btn.onclick = () => { curMpp = m; buildZoombar(); buildMatrix(); render(); };
    bar.appendChild(btn);
  }
}

buildZoombar();
buildMatrix();
buildAddBar();
// Default: central + NE San Francisco, so the bay is in frame to show water.
setBbox(37.762, -122.435, 37.808, -122.388);
if (lmap) lmap.fitBounds([[37.762, -122.435], [37.808, -122.388]], { padding: [20, 20] });
render();
