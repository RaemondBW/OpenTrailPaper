// Map style editor: fetch OSM roads for an area (reusing the tile builder's
// Overpass fetch + road classifier), then preview them rendered device-style at
// each zoom level with a configurable per-zoom "what's shown + how thick" table.
import { fetchOverpass, classify } from "./mapgen.js";

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

$("load").onclick = async () => {
  const lat = parseFloat($("lat").value), lon = parseFloat($("lon").value);
  const km = Math.max(1, Math.min(12, parseFloat($("km").value) || 5));
  const dLat = (km / 2) / 111.0;
  const dLon = (km / 2) / (111.0 * Math.cos(lat * Math.PI / 180));
  const bb = { s: lat - dLat, w: lon - dLon, n: lat + dLat, e: lon + dLon };
  $("load").disabled = true;
  setStatus("Downloading OSM data…");
  try {
    const json = await fetchOverpass(bb, (m) => setStatus(m));
    parse(json);
    setStatus(`Loaded ${roads.length} roads, ${waterPolys.length} water polygons.`);
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

function parse(json) {
  const nodes = new Map();
  for (const el of json.elements) if (el.type === "node") nodes.set(el.id, [el.lat, el.lon]);
  roads = []; waterPolys = [];
  let sumLa = 0, sumLo = 0, n = 0;
  for (const el of json.elements) {
    if (el.type !== "way" || !el.nodes) continue;
    const pts = el.nodes.map((id) => nodes.get(id)).filter(Boolean);
    if (pts.length < 2) continue;
    const tags = el.tags || {};
    if (tags.natural === "water") { waterPolys.push(pts); continue; }
    if (tags.natural === "coastline") continue;
    const cls = classify(tags);
    if (cls == null) continue;
    roads.push({ cls, pts });
    for (const p of pts) { sumLa += p[0]; sumLo += p[1]; n++; }
  }
  center = n ? [sumLa / n, sumLo / n] : null;
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
    ctx.fillStyle = "#e2e2e2";
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
render();
