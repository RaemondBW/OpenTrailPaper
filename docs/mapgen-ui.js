// DOM wiring for the in-browser offline-map generator. The encoding lives in
// mapgen.js (a pure, testable library); this file is just the map picker + form.
import { buildEbm, headerOnlySize, fetchOverpass } from "./mapgen.js";

const $ = (id) => document.getElementById(id);
const fieldN = $("bb-n"), fieldS = $("bb-s"), fieldW = $("bb-w"), fieldE = $("bb-e");
const infoEl = $("bb-info");
const genBtn = $("gen-btn"), genStatus = $("gen-status"), genLog = $("gen-log");
const genDownload = $("gen-download");
const barWrap = document.querySelector("#maps .progress");
const drawBtn = $("drawbox"), useViewBtn = $("useview");

// The generator section is optional on the page; bail cleanly if it's absent.
if (genBtn) init();

function init() {
  let map = null, rect = null, drawing = false, downloadUrl = null;

  // --- Leaflet map (optional — the numeric fields work without it) ----------
  if (typeof L !== "undefined") {
    map = L.map("pickmap", { zoomControl: true }).setView([37.7625, -122.44], 12);
    L.tileLayer("https://tile.openstreetmap.org/{z}/{x}/{y}.png", {
      maxZoom: 19,
      attribution: "&copy; OpenStreetMap contributors",
    }).addTo(map);
    // Recompute size once laid out (in case the section was display:none-ish).
    setTimeout(() => map.invalidateSize(), 200);

    useViewBtn.addEventListener("click", () => {
      const b = map.getBounds();
      setBbox(b.getSouth(), b.getWest(), b.getNorth(), b.getEast(), false);
    });

    // Drag-to-draw a rectangle. Toggling the mode disables map panning so the
    // drag paints a box instead of moving the map.
    drawBtn.addEventListener("click", () => setDrawing(!drawing));

    let dragStart = null;
    map.on("mousedown", (e) => {
      if (!drawing) return;
      dragStart = e.latlng;
      L.DomEvent.preventDefault(e.originalEvent);
    });
    map.on("mousemove", (e) => {
      if (!drawing || !dragStart) return;
      drawRect(L.latLngBounds(dragStart, e.latlng));
    });
    map.on("mouseup", (e) => {
      if (!drawing || !dragStart) return;
      const b = L.latLngBounds(dragStart, e.latlng);
      dragStart = null;
      setDrawing(false);
      setBbox(b.getSouth(), b.getWest(), b.getNorth(), b.getEast(), false);
    });
  } else {
    // No Leaflet (offline / blocked CDN): hide map affordances, keep the form.
    const pm = $("pickmap");
    if (pm) pm.style.display = "none";
    drawBtn.style.display = "none";
    useViewBtn.style.display = "none";
  }

  function setDrawing(on) {
    drawing = on;
    drawBtn.classList.toggle("active", on);
    if (!map) return;
    const el = map.getContainer();
    el.style.cursor = on ? "crosshair" : "";
    if (on) map.dragging.disable();
    else map.dragging.enable();
  }

  function drawRect(bounds) {
    if (!map) return;
    if (!rect) rect = L.rectangle(bounds, { color: "#c65a1e", weight: 2, fillOpacity: 0.08 }).addTo(map);
    else rect.setBounds(bounds);
  }

  // --- bbox state <-> fields -----------------------------------------------
  function setBbox(s, w, n, e, fit) {
    fieldS.value = round5(s); fieldW.value = round5(w);
    fieldN.value = round5(n); fieldE.value = round5(e);
    onFieldsChanged(fit);
  }

  function readFields() {
    const s = parseFloat(fieldS.value), w = parseFloat(fieldW.value);
    const n = parseFloat(fieldN.value), e = parseFloat(fieldE.value);
    if ([s, w, n, e].some((v) => !isFinite(v))) return null;
    return { s, w, n, e };
  }

  function onFieldsChanged(fit) {
    const bb = readFields();
    if (!bb) {
      genBtn.disabled = true;
      infoEl.textContent = "Enter or draw a bounding box.";
      infoEl.className = "bbox-info";
      return;
    }
    if (bb.s >= bb.n || bb.w >= bb.e) {
      genBtn.disabled = true;
      infoEl.textContent = "North must be above south and east must be right of west.";
      infoEl.className = "bbox-info warn";
      return;
    }
    if (map) {
      drawRect(L.latLngBounds([bb.s, bb.w], [bb.n, bb.e]));
      if (fit) map.fitBounds(rect.getBounds(), { padding: [20, 20] });
    }
    // Rough footprint so people don't ask for an entire country.
    const midLat = (bb.s + bb.n) / 2;
    const kmW = (bb.e - bb.w) * 111.32 * Math.cos((midLat * Math.PI) / 180);
    const kmH = (bb.n - bb.s) * 110.57;
    const areaKm2 = Math.round(kmW * kmH);
    let msg = `Selected ≈ ${kmW.toFixed(0)} × ${kmH.toFixed(0)} km (${areaKm2.toLocaleString()} km²).`;
    if (areaKm2 > 40000) {
      genBtn.disabled = true;
      infoEl.textContent = msg + " Too large — draw a smaller area (Overpass will refuse it).";
      infoEl.className = "bbox-info warn";
      return;
    }
    genBtn.disabled = false;
    infoEl.className = "bbox-info";
    infoEl.textContent = areaKm2 > 6000
      ? msg + " Large — the download may take a minute or two."
      : msg;
  }

  [fieldS, fieldW, fieldN, fieldE].forEach((f) =>
    f.addEventListener("input", () => onFieldsChanged(false)));

  // --- generate -------------------------------------------------------------
  function log(msg) { genLog.textContent += msg + "\n"; genLog.scrollTop = genLog.scrollHeight; }
  function setStatus(text, kind) { genStatus.textContent = text; genStatus.className = "status" + (kind ? " " + kind : ""); }

  // --- minimal store-only ZIP (so the download drops straight onto the SD) ---
  const _crcT = (() => {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
      t[n] = c >>> 0;
    }
    return t;
  })();
  function crc32(buf) {
    let c = 0xFFFFFFFF;
    for (let i = 0; i < buf.length; i++) c = _crcT[(c ^ buf[i]) & 0xFF] ^ (c >>> 8);
    return (c ^ 0xFFFFFFFF) >>> 0;
  }
  function zipStore(files) {
    const enc = new TextEncoder();
    const u16 = (v) => new Uint8Array([v & 255, (v >> 8) & 255]);
    const u32 = (v) => new Uint8Array([v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255]);
    const parts = [], central = [];
    let offset = 0;
    for (const f of files) {
      const nb = enc.encode(f.name), crc = crc32(f.data), sz = f.data.length;
      const lfh = [u32(0x04034b50), u16(20), u16(0), u16(0), u16(0), u16(0),
                   u32(crc), u32(sz), u32(sz), u16(nb.length), u16(0), nb];
      lfh.forEach((p) => parts.push(p));
      parts.push(f.data);
      central.push([u32(0x02014b50), u16(20), u16(20), u16(0), u16(0), u16(0), u16(0),
                    u32(crc), u32(sz), u32(sz), u16(nb.length), u16(0), u16(0), u16(0),
                    u16(0), u32(0), u32(offset), nb]);
      offset += 30 + nb.length + sz;
    }
    const cStart = offset;
    let cSize = 0;
    central.forEach((e) => e.forEach((p) => { parts.push(p); cSize += p.length; }));
    [u32(0x06054b50), u16(0), u16(0), u16(files.length), u16(files.length),
     u32(cSize), u32(cStart), u16(0)].forEach((p) => parts.push(p));
    let total = 0;
    parts.forEach((p) => (total += p.length));
    const out = new Uint8Array(total);
    let o = 0;
    parts.forEach((p) => { out.set(p, o); o += p.length; });
    return out;
  }

  genBtn.addEventListener("click", async () => {
    const bb = readFields();
    if (!bb) return;
    // Auto-named from the box centre so several regions never collide in /maps.
    const latC = ((bb.s + bb.n) / 2).toFixed(3), lonC = ((bb.w + bb.e) / 2).toFixed(3);
    const name = `map_${latC}_${lonC}`.replace(/[^A-Za-z0-9._-]/g, "_");

    genBtn.disabled = true;
    genDownload.hidden = true;
    if (downloadUrl) { URL.revokeObjectURL(downloadUrl); downloadUrl = null; }
    genLog.textContent = "";
    barWrap.hidden = false;
    barWrap.classList.add("indet");
    setStatus("Downloading map data…");
    log(`Bounding box  S ${bb.s}  W ${bb.w}  N ${bb.n}  E ${bb.e}`);

    try {
      const json = await fetchOverpass(bb, (s) => { setStatus("Downloading map data…"); log(s); });
      log(`Got ${json.elements.length.toLocaleString()} OSM elements. Building…`);
      setStatus("Building map…");
      // Yield so the status paints before the (synchronous) encode.
      await new Promise((r) => setTimeout(r, 0));

      const ebm = buildEbm(json, bb);
      if (ebm.length <= headerOnlySize(bb.s, bb.w, bb.n, bb.e)) {
        throw new Error("No roads found in that area — try a different or larger box.");
      }

      // Package as a ZIP laid out for the SD card: unzip onto the card root and
      // it lands at /maps/<name>.ebm.
      const zip = zipStore([{ name: `maps/${name}.ebm`, data: ebm }]);
      const blob = new Blob([zip], { type: "application/zip" });
      downloadUrl = URL.createObjectURL(blob);
      genDownload.href = downloadUrl;
      genDownload.download = `bikegps-${name}.zip`;
      genDownload.textContent = `⬇ Download ZIP (${fmtBytes(zip.length)}) — unzip onto the SD card`;
      genDownload.hidden = false;
      log(`Done: ${fmtBytes(ebm.length)} map. Unzip onto the SD card root → /maps/${name}.ebm.`);
      setStatus("Map ready — download the ZIP below.", "ok");
    } catch (err) {
      log("Error: " + (err && err.message ? err.message : String(err)));
      setStatus(err && err.message ? err.message : "Failed.", "err");
    } finally {
      barWrap.hidden = true;
      barWrap.classList.remove("indet");
      genBtn.disabled = false;
    }
  });

  onFieldsChanged(false);
}

function round5(v) { return Math.round(v * 1e5) / 1e5; }
function fmtBytes(n) {
  if (n < 1024) return n + " B";
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + " KB";
  return (n / (1024 * 1024)).toFixed(2) + " MB";
}
