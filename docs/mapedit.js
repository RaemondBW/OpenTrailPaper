// Map style editor: fetch OSM roads for an area (reusing the tile builder's
// Overpass fetch + road classifier), then preview them rendered device-style at
// each zoom level with a configurable per-zoom "what's shown + how thick" table.
import { fetchOverpass, classify, assembleCoastline, regionSeaPolygons } from "./mapgen.js";

const ZOOMS = [1, 2, 4, 8, 16, 32];   // m/px, the device's zoom stops
const TIERS = [
  { cls: 0, key: "arterial",  label: "Arterial",  hint: "motorway · trunk" },
  { cls: 1, key: "secondary", label: "Secondary", hint: "primary · secondary · tertiary" },
  { cls: 2, key: "minor",     label: "Minor",     hint: "residential · unclassified · …" },
  { cls: 3, key: "path",      label: "Path",      hint: "footway · cycleway · track · steps" },
];
// Device map area is 540×770 px; render at 0.7× so it fits on the page.
const DEV_W = 540, DEV_H = 770, SC = 0.7;

let cfg = defaultConfig();
let roads = [], waterPolys = [], center = null, curMpp = 8;

// Defaults that match the firmware: path shed at ≥4 m/px, minor at ≥8,
// secondary at ≥16, arterial never; arterial thins from width 5 to 2 at ≥16.
function defaultConfig() {
  const c = { tiers: {}, water: {} };
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

const $ = (id) => document.getElementById(id);
const cv = $("cv"), ctx = cv.getContext("2d");
const setStatus = (m) => { $("status").textContent = m; };

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

$("load").onclick = async () => {
  const bb = readBbox();
  if (!bb) { setStatus("Draw or enter a valid box (north above south, east right of west)."); return; }
  const midLat = (bb.s + bb.n) / 2;
  const kmW = (bb.e - bb.w) * 111.32 * Math.cos(midLat * Math.PI / 180);
  const kmH = (bb.n - bb.s) * 110.57;
  if (kmW * kmH > 40000) { setStatus("Too large — draw a smaller box (Overpass will refuse it)."); return; }
  $("load").disabled = true;
  setStatus("Downloading OSM data…");
  try {
    const json = await fetchOverpass(bb, (m) => setStatus(m));
    parse(json, bb);
    center = [midLat, (bb.w + bb.e) / 2];   // preview centred on the selected box
    setStatus(`Loaded ${roads.length} roads, ${waterPolys.length} water polygons · ${kmW.toFixed(1)}×${kmH.toFixed(1)} km.`);
    render();
  } catch (e) {
    setStatus("Failed: " + (e && e.message ? e.message : e));
  }
  $("load").disabled = false;
};

$("reset").onclick = () => { cfg = defaultConfig(); buildMatrix(); render(); };
$("export").onclick = () => {
  const out = $("out"); out.hidden = false;
  out.textContent = JSON.stringify(cfg, null, 2);
};

function parse(json, bb) {
  const nodes = new Map();
  for (const el of json.elements) if (el.type === "node") nodes.set(el.id, [el.lat, el.lon]);
  roads = []; waterPolys = [];
  const coastWays = [];
  for (const el of json.elements) {
    if (el.type !== "way" || !el.nodes) continue;
    const tags = el.tags || {};
    if (tags.natural === "coastline") { coastWays.push(el.nodes); continue; }
    const pts = el.nodes.map((id) => nodes.get(id)).filter(Boolean);
    if (pts.length < 2) continue;
    if (tags.natural === "water") { waterPolys.push(pts); continue; }
    const cls = classify(tags);
    if (cls == null) continue;
    roads.push({ cls, pts });
  }
  // Ocean/bay come from natural=coastline, not natural=water — assemble the
  // coastline into sea polygons for the box (the same region-level fill the
  // tile builder uses) so the big water shows, not just inland lakes.
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

  if (cfg.water[curMpp]) {
    ctx.fillStyle = "#cfd6dd";   // light slate — reads as water on the white map
    for (const poly of waterPolys) {
      ctx.beginPath();
      poly.forEach((p, i) => { const q = proj(p); i ? ctx.lineTo(q[0], q[1]) : ctx.moveTo(q[0], q[1]); });
      ctx.closePath(); ctx.fill();
    }
  }

  // Draw path first (under), arterial last (on top) — the device's order.
  for (const t of [...TIERS].reverse()) {
    const c = cfg.tiers[t.key][curMpp];
    if (!c.show) continue;
    ctx.strokeStyle = t.cls === 3 ? "#b0b0b0" : "#111";   // paths read as light grey
    ctx.lineWidth = Math.max(0.5, c.width * SC);
    ctx.lineJoin = "round"; ctx.lineCap = "round";
    ctx.beginPath();
    for (const road of roads) {
      if (road.cls !== t.cls) continue;
      road.pts.forEach((p, i) => { const q = proj(p); i ? ctx.lineTo(q[0], q[1]) : ctx.moveTo(q[0], q[1]); });
    }
    ctx.stroke();
  }
}

function buildMatrix() {
  let html = "<tr><th class='tier'>Tier</th>";
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
  $("matrix").innerHTML = html;

  $("matrix").querySelectorAll("input[type=checkbox][data-t]").forEach((el) =>
    (el.onchange = () => { cfg.tiers[el.dataset.t][+el.dataset.m].show = el.checked; render(); }));
  $("matrix").querySelectorAll("input[data-tw]").forEach((el) =>
    (el.onchange = () => { cfg.tiers[el.dataset.tw][+el.dataset.m].width = Math.max(1, +el.value || 1); render(); }));
  $("matrix").querySelectorAll("input[data-water]").forEach((el) =>
    (el.onchange = () => { cfg.water[+el.dataset.water] = el.checked; render(); }));
}

function buildZoombar() {
  const bar = $("zoombar");
  bar.innerHTML = "";
  for (const m of ZOOMS) {
    const b = document.createElement("button");
    b.textContent = m + " m/px";
    if (m === curMpp) b.className = "active";
    b.onclick = () => { curMpp = m; buildZoombar(); buildMatrix(); render(); };
    bar.appendChild(b);
  }
}

buildZoombar();
buildMatrix();
// Default: central + NE San Francisco, so the bay is in frame to show water.
setBbox(37.762, -122.435, 37.808, -122.388);
if (lmap) lmap.fitBounds([[37.762, -122.435], [37.808, -122.388]], { padding: [20, 20] });
render();
