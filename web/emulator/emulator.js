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
      if (rx[2] === 0xf6) { whiten(); rx = rx.subarray(3); continue; }
      rx = rx.subarray(1); continue;
    }
    if (rx[1] !== 0x46 /* 'F' */) { rx = rx.subarray(1); continue; }
    if (rx.length < 8) return;
    const rleLen = rx[4] | (rx[5] << 8) | (rx[6] << 16) | (rx[7] << 24);
    if (rx.length < 8 + rleLen + 1) return;   // wait for the trailer
    if (rx[8 + rleLen] !== 0xf6) { rx = rx.subarray(1); continue; }
    drawFrame(rx.subarray(8, 8 + rleLen));
    rx = rx.subarray(8 + rleLen + 1);
    frames++;
    setStatus(`connected · frame ${frames}`);
  }
}

const fb = new Uint8Array(FB_BYTES);

function drawFrame(rle) {
  let o = 0;
  for (let i = 0; i + 1 < rle.length && o < FB_BYTES; i += 2) {
    fb.fill(rle[i + 1], o, Math.min(o + rle[i], FB_BYTES));
    o += rle[i];
  }
  const px = image.data;
  for (let y = 0; y < NATIVE_H; y++) {
    for (let x = 0; x < NATIVE_W; x++) {
      const b = fb[(y * NATIVE_W + x) >> 1];
      const v = (x & 1) ? (b >> 4) : (b & 0x0f);
      const c = LUT[v];
      // portrait: row = W-1-x, col = y
      const d = ((NATIVE_W - 1 - x) * NATIVE_H + y) * 4;
      px[d] = c[0]; px[d + 1] = c[1]; px[d + 2] = c[2]; px[d + 3] = 255;
    }
  }
  ctx.putImageData(image, 0, 0);
}

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

function wireButton(id, key) {
  const el = document.getElementById(id);
  const down = (e) => { e.preventDefault(); send([0xe1, key, 1]); };
  const up = () => send([0xe1, key, 0]);
  el.addEventListener("pointerdown", down);
  el.addEventListener("pointerup", up);
  el.addEventListener("pointerleave", up);
}
wireButton("btn-boot", 1);
wireButton("btn-side", 2);

document.getElementById("btn-home").addEventListener("pointerdown", (e) => {
  e.preventDefault();
  send([0xe4]);
});

function panelXY(e) {
  const r = canvas.getBoundingClientRect();
  const x = Math.round((e.clientX - r.left) / r.width * 540);
  const y = Math.round((e.clientY - r.top) / r.height * 960);
  return [Math.max(0, Math.min(539, x)), Math.max(0, Math.min(959, y))];
}

let touching = false;
canvas.addEventListener("pointerdown", (e) => {
  e.preventDefault();
  touching = true;
  const [x, y] = panelXY(e);
  send([0xe2, x & 0xff, x >> 8, y & 0xff, y >> 8]);
});
canvas.addEventListener("pointermove", (e) => {
  if (!touching) return;
  const [x, y] = panelXY(e);
  send([0xe2, x & 0xff, x >> 8, y & 0xff, y >> 8]);
});
for (const ev of ["pointerup", "pointerleave", "pointercancel"]) {
  canvas.addEventListener(ev, () => {
    if (touching) { touching = false; send([0xe3]); }
  });
}

// ---------------------------------------------------------------------------
// Ride simulator: 1 Hz NMEA, carried as 0xE5 chunks on the UART1 wire — the
// QEMU esp32s3 machine wires no third serial, so the firmware loops these
// into the GPS UART itself. A gentle loop around Golden Gate Park at ~22 km/h.
// ---------------------------------------------------------------------------

let simTimer = null;
document.getElementById("ride-sim").addEventListener("change", (e) => {
  if (e.target.checked) {
    let angle = 0;
    simTimer = setInterval(() => {
      angle += 0.0035;
      const lat = 37.7694 + 0.0045 * Math.sin(angle);
      const lon = -122.4862 + 0.0090 * Math.cos(angle);
      sendNmea(lat, lon, 22.0, (angle * 180 / Math.PI) % 360);
    }, 1000);
  } else {
    clearInterval(simTimer);
    simTimer = null;
  }
});

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
      send([0xe6, hr, pwr & 0xff, pwr >> 8, cad]);
      send([0xe7, +sliders.bat.value, 0]);
    }, 1000);
  } else {
    clearInterval(sensorTimer);
    sensorTimer = null;
    send([0xe6, 0xff, 0xff, 0xff, 0xff]);   // all absent
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
  const rmc = withChecksum(
    `GPRMC,${t},A,${latS},${lonS},${knots},${course.toFixed(1)},${date},,,A`);
  const payload = new TextEncoder().encode(gga + rmc);
  // 0xE5-framed chunks on the UART1 wire, <=255 bytes each.
  for (let i = 0; i < payload.length; i += 255) {
    const chunk = payload.subarray(i, Math.min(i + 255, payload.length));
    send([0xe5, chunk.length, ...chunk]);
  }
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

function connect() {
  ws = new WebSocket(`ws://${location.host}/ws`);
  ws.binaryType = "arraybuffer";
  ws.onopen = () => setStatus("connected · waiting for a frame");
  ws.onclose = () => {
    setStatus("bridge closed — is serve.py (and QEMU) running?", true);
    setTimeout(connect, 2000);
  };
  ws.onmessage = (e) => {
    const data = new Uint8Array(e.data);
    if (data.length < 2) return;
    if (data[0] === 0) feed(data.subarray(1));
    // channel 1 (GPS echo) is uninteresting to the page
  };
}

whiten();
connect();
