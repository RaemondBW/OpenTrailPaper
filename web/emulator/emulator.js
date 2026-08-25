// The page half of the emulator: draws UART1 frames on the panel canvas and
// turns the device model's buttons and glass into UART1 input events. The
// transport is a channel-tagged WebSocket (tools/emu/serve.py) in phase 2 and
// becomes an in-page pipe when QEMU itself compiles to WASM in phase 3 — the
// protocol on either side of it never changes.
//
// Wire protocol: see src/epd_compat_emu.cpp. Native framebuffer is 960x540
// landscape, 4bpp, low nibble = even x; the UI lays out portrait, native
// (x,y) -> portrait row (W-1-x), col y — same math as tools/emu/frame2png.py.

"use strict";

const NATIVE_W = 960, NATIVE_H = 540;
const FB_BYTES = NATIVE_W * NATIVE_H / 2;
const PAPER = [0xf4, 0xf1, 0xea], INK = [0x14, 0x13, 0x12];

const canvas = document.getElementById("panel");
const ctx = canvas.getContext("2d");
const statusEl = document.getElementById("status");
const image = ctx.createImageData(540, 960);

// 16 e-paper tones, precomputed ink->paper.
const LUT = [];
for (let v = 0; v < 16; v++) {
  const t = v / 15;
  LUT.push([
    INK[0] + (PAPER[0] - INK[0]) * t,
    INK[1] + (PAPER[1] - INK[1]) * t,
    INK[2] + (PAPER[2] - INK[2]) * t,
  ]);
}

let ws = null, frames = 0;

function setStatus(text, err = false) {
  statusEl.textContent = text;
  statusEl.classList.toggle("err", err);
}

// ---------------------------------------------------------------------------
// Frame stream: accumulate channel-0 bytes, cut frames out of the buffer.
// ---------------------------------------------------------------------------

let rx = new Uint8Array(0);

function feed(chunk) {
  const merged = new Uint8Array(rx.length + chunk.length);
  merged.set(rx); merged.set(chunk, rx.length);
  rx = merged;

  for (;;) {
    const start = rx.indexOf(0xf5);
    if (start < 0) { rx = new Uint8Array(0); return; }
    if (start > 0) rx = rx.subarray(start);
    if (rx.length < 2) return;

    if (rx[1] === 0x43 /* 'C' */) {          // clear
      if (rx.length < 3) return;
      if (rx[2] === 0xf6) { if (!onMap()) whiten(); rx = rx.subarray(3); continue; }
      rx = rx.subarray(1); continue;
    }
    if (rx[1] === 0x50 /* 'P' */) {          // view marker: 0xF5 'P' <code> 0xF6
      if (rx.length < 4) return;
      if (rx[3] === 0xf6) { setView(rx[2]); rx = rx.subarray(4); continue; }
      rx = rx.subarray(1); continue;
    }
    if (rx[1] === 0x4d /* 'M' */) {          // map state: 0xF5 'M' <mpp> <up> 0xF6
      if (rx.length < 5) return;
      if (rx[4] === 0xf6) { setMapState(rx[2], rx[3]); rx = rx.subarray(5); continue; }
      rx = rx.subarray(1); continue;
    }
    if (rx[1] !== 0x46 /* 'F' */) { rx = rx.subarray(1); continue; }
    if (rx.length < 8) return;
    const rleLen = rx[4] | (rx[5] << 8) | (rx[6] << 16) | (rx[7] << 24);
    if (rx.length < 8 + rleLen + 1) return;   // wait for the trailer
    if (rx[8 + rleLen] !== 0xf6) { rx = rx.subarray(1); continue; }
    // On the map page the firmware shows "NO MAP HERE" (QEMU has no PSRAM for the
    // projector). Ignore its frame — the WASM renderer draws the real map instead.
    if (!onMap()) {
      drawFrame(rx.subarray(8, 8 + rleLen));
      frames++;
      setStatus(`connected · frame ${frames}`);
    }
    rx = rx.subarray(8 + rleLen + 1);
  }
}

const fb = new Uint8Array(FB_BYTES);

function fillFbFromRle(rle) {
  let o = 0;
  for (let i = 0; i + 1 < rle.length && o < FB_BYTES; i += 2) {
    fb.fill(rle[i + 1], o, Math.min(o + rle[i], FB_BYTES));
    o += rle[i];
  }
}

// Blit the 4bpp landscape framebuffer (960x540) to the portrait canvas. Shared
// by firmware frames (via drawFrame) and the WASM map renderer, which produce a
// byte-identical fb.
function blitFb(src) {
  const px = image.data;
  for (let y = 0; y < NATIVE_H; y++) {
    for (let x = 0; x < NATIVE_W; x++) {
      const b = src[(y * NATIVE_W + x) >> 1];
      const v = (x & 1) ? (b >> 4) : (b & 0x0f);
      const c = LUT[v];
      // portrait: row = W-1-x, col = y
      const d = ((NATIVE_W - 1 - x) * NATIVE_H + y) * 4;
      px[d] = c[0]; px[d + 1] = c[1]; px[d + 2] = c[2]; px[d + 3] = 255;
    }
  }
  ctx.putImageData(image, 0, 0);
}

function drawFrame(rle) { fillFbFromRle(rle); blitFb(fb); }

// ---------------------------------------------------------------------------
// Map page — rendered IN THE BROWSER by the firmware's own renderer (map_wasm.js
// = map_tiles + ui_render + the epd_compat rasteriser, compiled to WASM with the
// SF map embedded). QEMU can't render it (its esp32s3 PSRAM model is broken and
// the projector needs PSRAM scratch), so on the map view we draw the WASM map
// from the same GPS/sensor state we feed the device. The firmware announces the
// active view over the 'P' marker.
// ---------------------------------------------------------------------------

let currentView = 0;
let mapMod = null, mapRender = null, mapFbPtr = 0, mapFbLen = 0, mapReady = false;
// Zoom + orientation, mirrored from the firmware (its own map buttons set these).
let mapZoom = 2.0;     // metres per pixel
let mapTrackUp = 0;    // 0 = north-up, 1 = track-up

// The live state the map chrome draws — updated by the ride/sensor sims below.
const simState = {
  lat: 37.7764, lon: -122.4346, course: 0, speed: 0, gpsFix: false,
  hr: 0, pwr: 0, cad: 0, hasHr: false, hasPwr: false, hasCad: false,
  bat: 76, distM: 0, rideSec: 0,
};

function onMap() { return currentView === 2 && mapReady; }

function setView(code) {
  const was = currentView;
  currentView = code;
  if (code === 2 && was !== 2) renderMap();   // draw the map the instant we enter
}

function setMapState(mpp, up) {
  mapZoom = mpp || 2;
  mapTrackUp = up ? 1 : 0;
  if (onMap()) renderMap();
}

if (typeof MapModule === "function") {
  MapModule().then((m) => {
    mapMod = m;
    if (!m.ccall("map_init", "number", [], [])) {
      console.warn("map_wasm: sf.ebm failed to load");
      return;
    }
    mapFbPtr = m.ccall("map_fb", "number", [], []);
    mapFbLen = m.ccall("map_fb_len", "number", [], []);
    mapRender = m.cwrap("map_render", null, Array(21).fill("number"));
    mapReady = true;
    if (onMap()) renderMap();
  });
}

function renderMap() {
  if (!mapReady) return;
  const s = simState;
  mapRender(s.lat, s.lon, mapZoom, s.course, mapTrackUp,
            s.bat, s.hr, s.pwr, s.cad,
            s.hasHr ? 1 : 0, s.hasPwr ? 1 : 0, s.hasCad ? 1 : 0,
            s.speed, s.distM, s.rideSec,
            Math.floor(Date.now() / 1000), -420, 1, s.gpsFix ? 1 : 0, 0);
  blitFb(mapMod.HEAPU8.subarray(mapFbPtr, mapFbPtr + mapFbLen));
  setStatus("connected · map (browser-rendered)");
}

// Repaint the map a few times a second while it's on screen so the rider marker
// and heading track the moving GPS fix.
setInterval(() => { if (onMap()) renderMap(); }, 300);

function whiten() {
  ctx.fillStyle = `rgb(${PAPER.join(",")})`;
  ctx.fillRect(0, 0, 540, 960);
}

// ---------------------------------------------------------------------------
// Input: buttons and glass -> UART1 events (channel 0).
// ---------------------------------------------------------------------------

function send(bytes) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(new Uint8Array([0, ...bytes]));
  }
}

// Visual confirmation that an input was sent — the on-device effect (a ride
// starting, the backlight) isn't always visible, so without this you can't tell
// a press registered. Flashes a labelled pill over the device at the button, and
// pulses the button nub itself.
const pressHud = document.createElement("div");
pressHud.id = "press-hud";
document.querySelector(".device")?.appendChild(pressHud);
function pressFeedback(el, label) {
  el.classList.add("pressed");
  setTimeout(() => el.classList.remove("pressed"), 180);
  if (label && pressHud) {
    pressHud.textContent = label;
    pressHud.classList.remove("show");   // restart the animation
    void pressHud.offsetWidth;
    pressHud.classList.add("show");
  }
}

function wireButton(id, key, label) {
  const el = document.getElementById(id);
  let held = false;
  const down = (e) => {
    e.preventDefault();
    // Capture the pointer so a hold keeps registering even if the cursor drifts
    // off the small nub — otherwise pointerleave released it early and a
    // long-press (BOOT hold -> power dialog) could never land.
    try { el.setPointerCapture(e.pointerId); } catch {}
    held = true;
    pressFeedback(el, label);
    send([0xe1, key, 1]);
  };
  const up = () => { if (held) { held = false; send([0xe1, key, 0]); } };
  el.addEventListener("pointerdown", down);
  el.addEventListener("pointerup", up);
  el.addEventListener("pointercancel", up);
}
wireButton("btn-boot", 1, "BOOT");
wireButton("btn-side", 2, "BACKLIGHT");

document.getElementById("btn-home").addEventListener("pointerdown", (e) => {
  e.preventDefault();
  pressFeedback(e.currentTarget, "HOME");
  send([0xe4]);
});

// Full reboot: the firmware calls esp_restart() (mailbox 0xEA), replaying the
// whole boot sequence — watch it in the panel and the devtools console.
document.getElementById("reboot-device").addEventListener("click", () => {
  send([0xea]);
  frames = 0;
  setStatus("rebooting…");
});

// Inject + start a sample structured workout (mailbox 0xE9). Visible on the
// workout page — swipe the glass to reach it.
document.getElementById("load-workout").addEventListener("click", () => {
  send([0xe9]);
  setStatus("workout loaded · swipe to the workout page");
});

function panelXY(e) {
  const r = canvas.getBoundingClientRect();
  const x = Math.round((e.clientX - r.left) / r.width * 540);
  const y = Math.round((e.clientY - r.top) / r.height * 960);
  return [Math.max(0, Math.min(539, x)), Math.max(0, Math.min(959, y))];
}

// Touch model: a TAP is one atomic event (0xEC x,y) sent on release, so the
// firmware never has to correlate a separate down and up that the transport
// might deliver apart, batch together, or drop — the source of the "buttons do
// nothing" flakiness. A DRAG (moved far enough) instead streams 0xE2 move events
// for map panning and ends with 0xE3, and sends no tap. Every press also shows a
// ripple on the glass so you can see it registered.
let touchDown = null;   // {x, y} in panel coords, or null
let touchMoved = false;
const TAP_SLOP = 16;    // panel px of movement still counted as a tap

canvas.addEventListener("pointerdown", (e) => {
  e.preventDefault();
  try { canvas.setPointerCapture(e.pointerId); } catch {}
  const [x, y] = panelXY(e);
  touchDown = { x, y };
  touchMoved = false;
  rippleAt(e);
});
canvas.addEventListener("pointermove", (e) => {
  if (!touchDown) return;
  const [x, y] = panelXY(e);
  if (Math.abs(x - touchDown.x) > TAP_SLOP || Math.abs(y - touchDown.y) > TAP_SLOP) {
    if (!touchMoved) {                       // first real movement: begin a drag
      touchMoved = true;
      send([0xe2, touchDown.x & 0xff, touchDown.x >> 8,
            touchDown.y & 0xff, touchDown.y >> 8]);
    }
    send([0xe2, x & 0xff, x >> 8, y & 0xff, y >> 8]);
  }
});
for (const ev of ["pointerup", "pointercancel"]) {
  canvas.addEventListener(ev, (e) => {
    if (!touchDown) return;
    const d = touchDown;
    touchDown = null;
    if (touchMoved) {
      send([0xe3]);                          // end the drag
    } else {
      send([0xec, d.x & 0xff, d.x >> 8, d.y & 0xff, d.y >> 8]);  // atomic tap
    }
  });
}

// A quick expanding ring at the press point, drawn over the panel canvas, so a
// press is visibly acknowledged even before the device repaints.
const rippleLayer = document.createElement("div");
rippleLayer.id = "ripple-layer";
canvas.parentElement?.appendChild(rippleLayer);
function rippleAt(e) {
  if (!rippleLayer) return;
  const r = canvas.getBoundingClientRect();
  const host = rippleLayer.getBoundingClientRect();
  const dot = document.createElement("span");
  dot.className = "ripple";
  dot.style.left = (e.clientX - host.left) + "px";
  dot.style.top = (e.clientY - host.top) + "px";
  rippleLayer.appendChild(dot);
  setTimeout(() => dot.remove(), 500);
}

// ---------------------------------------------------------------------------
// Ride simulator: 1 Hz NMEA, carried as 0xE5 chunks on the UART1 wire — the
// QEMU esp32s3 machine wires no third serial, so the firmware loops these
// into the GPS UART itself. A gentle loop around Golden Gate Park at ~22 km/h.
// ---------------------------------------------------------------------------

let simTimer = null, simAngle = 0, lastGpsMs = 0;
const KMH = 22.0;
// The firmware drops the fix if the newest position is older than 3 s
// (gps.location.age() < 3000). Feed comfortably inside that — 750 ms leaves a
// 4-beat margin so ordinary timer jitter never loses the fix.
const GPS_MS = 750;

function gpsTick(advance = true) {
  if (advance) simAngle += 0.0035 * (GPS_MS / 1000);
  // A gentle loop through the dense Alamo Square / Panhandle grid, which the
  // embedded SF map covers richly.
  const lat = 37.7764 + 0.0035 * Math.sin(simAngle);
  const lon = -122.4346 + 0.0070 * Math.cos(simAngle);
  const course = (simAngle * 180 / Math.PI + 90) % 360;
  sendNmea(lat, lon, KMH, course);
  simState.lat = lat; simState.lon = lon; simState.course = course;
  simState.speed = KMH; simState.gpsFix = true;
  if (advance) {
    simState.distM += KMH / 3.6 * (GPS_MS / 1000);
    simState.rideSec = Math.round(simState.rideSec + GPS_MS / 1000);
  }
  lastGpsMs = perfNow();
}

document.getElementById("ride-sim").addEventListener("change", (e) => {
  if (e.target.checked) {
    gpsTick(false);                       // a fix immediately, don't wait a beat
    simTimer = setInterval(gpsTick, GPS_MS);
  } else {
    clearInterval(simTimer);
    simTimer = null;
    simState.gpsFix = false;
    simState.speed = 0;
  }
});

// A backgrounded tab has its timers throttled, so the GPS feed stalls and the
// firmware drops the fix. The moment the tab is visible again, feed a fresh fix
// right away (without waiting for the throttled interval to catch up) so the fix
// recovers instantly instead of showing "searching".
document.addEventListener("visibilitychange", () => {
  if (!document.hidden && simTimer) gpsTick(false);
});
function perfNow() { try { return performance.now(); } catch { return 0; } }

// ---------------------------------------------------------------------------
// Sensor spoofing: sliders -> 0xE6 (hr/power/cadence) at 1 Hz with a little
// life added, plus 0xE7 battery. 0xFF/0xFFFF = "sensor absent" when off.
// ---------------------------------------------------------------------------

const sliders = {};
for (const id of ["hr", "pwr", "cad", "bat"]) {
  const input = document.getElementById(`s-${id}`);
  const out = document.getElementById(`o-${id}`);
  input.addEventListener("input", () => { out.value = input.value; });
  sliders[id] = input;
}

let sensorTimer = null;
document.getElementById("sensor-sim").addEventListener("change", (e) => {
  document.getElementById("sensor-sliders").hidden = !e.target.checked;
  if (e.target.checked) {
    sensorTimer = setInterval(() => {
      const jitter = (v, j) => Math.max(0, Math.round(+v + (Math.random() * 2 - 1) * j));
      const hr = Math.min(254, jitter(sliders.hr.value, 2));
      const pwr = jitter(sliders.pwr.value, 12);
      const cad = Math.min(254, jitter(sliders.cad.value, 3));
      // One WebSocket message = one mailbox push = one brief guest halt, so
      // pack both events together rather than sending two.
      send([0xe6, hr, pwr & 0xff, pwr >> 8, cad, 0xe7, +sliders.bat.value, 0]);
      Object.assign(simState, { hr, pwr, cad, bat: +sliders.bat.value,
                                hasHr: true, hasPwr: true, hasCad: true });
    }, 1000);
  } else {
    clearInterval(sensorTimer);
    sensorTimer = null;
    send([0xe6, 0xff, 0xff, 0xff, 0xff]);   // all absent
    Object.assign(simState, { hasHr: false, hasPwr: false, hasCad: false });
  }
});

function nmeaCoord(deg, isLat) {
  const abs = Math.abs(deg);
  const d = Math.floor(abs);
  const m = (abs - d) * 60;
  const dd = String(d).padStart(isLat ? 2 : 3, "0");
  return `${dd}${m.toFixed(4).padStart(7, "0")}`;
}

function withChecksum(body) {
  let ck = 0;
  for (const ch of body) ck ^= ch.charCodeAt(0);
  return `$${body}*${ck.toString(16).toUpperCase().padStart(2, "0")}\r\n`;
}

function sendNmea(lat, lon, kmh, course) {
  const now = new Date();
  const t = now.toISOString().slice(11, 19).replaceAll(":", "") + ".00";
  const d = now.toISOString().slice(2, 10).split("-");
  const date = d[2] + d[1] + d[0];
  const latS = `${nmeaCoord(lat, true)},${lat >= 0 ? "N" : "S"}`;
  const lonS = `${nmeaCoord(lon, false)},${lon >= 0 ? "E" : "W"}`;
  const knots = (kmh / 1.852).toFixed(1);
  const gga = withChecksum(
    `GPGGA,${t},${latS},${lonS},1,08,1.2,30.0,M,0.0,M,,`);
  // GSA carries the fix TYPE (field 2 = 3 -> 3D fix). Without it the firmware's
  // GPS parser leaves fix type at 0 (no fix) and the ride never leaves the
  // auto-paused state, so ride time/distance stay at zero despite a position.
  const gsa = withChecksum(`GPGSA,A,3,01,02,03,04,05,06,07,08,,,,,1.8,1.2,1.2`);
  const rmc = withChecksum(
    `GPRMC,${t},A,${latS},${lonS},${knots},${course.toFixed(1)},${date},,,A`);
  const payload = new TextEncoder().encode(gga + gsa + rmc);
  // 0xE5-framed chunks on the UART1 wire, <=255 bytes each.
  for (let i = 0; i < payload.length; i += 255) {
    const chunk = payload.subarray(i, Math.min(i + 255, payload.length));
    send([0xe5, chunk.length, ...chunk]);
  }
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

let didAutoReboot = false;   // reboot once per fresh page load, not on reconnects
function connect() {
  ws = new WebSocket(`ws://${location.host}/ws`);
  ws.binaryType = "arraybuffer";
  ws.onopen = () => {
    setStatus("connected · waiting for a frame");
    // Loading the page reboots the device, so you always start from a clean
    // power-on boot (skippable with ?noreboot). The reboot only drops the
    // firmware, not this WebSocket to serve.py, so onopen won't loop.
    if (!didAutoReboot && !/[?&]noreboot\b/.test(location.search)) {
      didAutoReboot = true;
      frames = 0;
      setStatus("rebooting on load…");
      send([0xea]);
    }
    // Ask the firmware to re-send the current screen — it only emits frames on
    // change, so a page that connects after the dashboard settled would
    // otherwise stay blank. Repeat a couple of times in case boot is mid-flight.
    const nudge = () => send([0xe8]);
    nudge();
    setTimeout(nudge, 800);
    setTimeout(nudge, 2000);
    if (window._repaintTimer) clearInterval(window._repaintTimer);
    window._repaintTimer = setInterval(nudge, 4000);
  };
  ws.onclose = () => {
    setStatus("bridge closed — is serve.py (and QEMU) running?", true);
    setTimeout(connect, 2000);
  };
  ws.onmessage = (e) => {
    const data = new Uint8Array(e.data);
    if (data.length < 2) return;
    if (data[0] === 0) feed(data.subarray(1));
    else if (data[0] === 2) logSerial(data.subarray(1));
    // channel 1 (GPS echo) is uninteresting to the page
  };
}

// The device's serial console (UART0) -> an on-page log panel (the "webpage
// console"). A bounded, auto-trimmed panel rather than a flood into the devtools
// console — thousands of telemetry lines in devtools with it open is itself a
// major source of lag. Rendering is batched on an animation frame so a burst of
// lines can't stall the main thread. Bytes arrive in arbitrary chunks, so buffer
// until a newline.
const serialDecoder = new TextDecoder();
let serialBuf = "";
const SERIAL_MAX_LINES = 250;
const serialLog = document.getElementById("serial-log");
const serialLines = [];
let serialDirty = false;

function flushSerial() {
  serialDirty = false;
  if (!serialLog) return;
  serialLog.textContent = serialLines.join("\n");
  // Keep pinned to the newest line unless the user has scrolled up to read.
  if (serialLog.dataset.pinned !== "0") serialLog.scrollTop = serialLog.scrollHeight;
}

function logSerial(bytes) {
  serialBuf += serialDecoder.decode(bytes, { stream: true });
  if (serialBuf.length > 65536) serialBuf = serialBuf.slice(-8192);
  let nl;
  while ((nl = serialBuf.indexOf("\n")) >= 0) {
    const line = serialBuf.slice(0, nl).replace(/\r$/, "");
    serialBuf = serialBuf.slice(nl + 1);
    if (!line) continue;
    if (line.startsWith("[FS]")) { fsLine(line.slice(4).trim()); continue; }
    serialLines.push(line);
    if (serialLines.length > SERIAL_MAX_LINES) serialLines.shift();
  }
  if (!serialDirty) { serialDirty = true; requestAnimationFrame(flushSerial); }
}

// ---------------------------------------------------------------------------
// Filesystem tab: the firmware reports its storage inventory (mailbox 0xEB) as
// "[FS]" console lines; we collect a BEGIN..END block and render it as a tree.
// ---------------------------------------------------------------------------
const fsView = document.getElementById("fs-view");
let fsPending = [];
function fsLine(rest) {
  if (rest === "BEGIN") { fsPending = []; return; }
  if (rest === "END") { renderFs(fsPending); fsPending = []; return; }
  fsPending.push(rest);
}
function renderFs(lines) {
  if (!fsView) return;
  const esc = (s) => s.replace(/[&<>]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" }[c]));
  let html = "";
  for (const l of lines) {
    // "mount /sd | <status>", "file <path> | <size> | <note>", "note <path> | <text>"
    const [kind, ...rest2] = l.split(" ");
    const parts = rest2.join(" ").split("|").map((s) => s.trim());
    if (kind === "mount") {
      html += `<div class="fs-status">${esc(parts[0])}: ${esc(parts[1] || "")}</div>`;
    } else if (kind === "file") {
      const size = parts[1] ? (Number(parts[1]) >= 1024
        ? (Number(parts[1]) / 1024).toFixed(0) + " KB" : parts[1] + " B") : "";
      html += `<div class="fs-row"><span class="fs-path">${esc(parts[0])}</span>` +
              `<span class="fs-meta">${esc(size)}${parts[2] ? " · " + esc(parts[2]) : ""}</span></div>`;
    } else if (kind === "note") {
      html += `<div class="fs-row"><span class="fs-path">${esc(parts[0])}</span>` +
              `<span class="fs-note">${esc(parts[1] || "")}</span></div>`;
    }
  }
  fsView.innerHTML = html || '<div class="fs-note">no inventory reported</div>';
}
function requestFsInventory() { send([0xeb]); }

// Tab switching.
for (const tab of document.querySelectorAll(".tab")) {
  tab.addEventListener("click", () => {
    const name = tab.dataset.tab;
    for (const t of document.querySelectorAll(".tab"))
      t.classList.toggle("active", t === tab);
    for (const p of document.querySelectorAll(".tabpane"))
      p.classList.toggle("active", p.dataset.tab === name);
    if (name === "filesystem") requestFsInventory();
  });
}
// Keep the filesystem view fresh while it's the visible tab.
setInterval(() => {
  const fsTab = document.querySelector('.tab[data-tab="filesystem"]');
  if (fsTab && fsTab.classList.contains("active")) requestFsInventory();
}, 2000);

if (serialLog) {
  // Remember whether the user scrolled up (so we stop auto-pinning to the end).
  serialLog.addEventListener("scroll", () => {
    const atBottom =
      serialLog.scrollHeight - serialLog.scrollTop - serialLog.clientHeight < 24;
    serialLog.dataset.pinned = atBottom ? "1" : "0";
  });
}
document.getElementById("serial-clear")?.addEventListener("click", () => {
  serialLines.length = 0;
  flushSerial();
});

whiten();
connect();
