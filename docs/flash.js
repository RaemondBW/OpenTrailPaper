// Web Serial firmware flasher for OpenTrailPaper.
//
// Uses esptool-js (the maintained successor to Adafruit_WebSerial_ESPTool).
// Pinned to a known version whose writeFlash() expects each fileArray entry's
// `data` as a *binary string* (it calls bstrToUi8 internally) — so files are
// read with readAsBinaryString, not as a Uint8Array. Everything runs locally;
// no binary ever leaves the browser.
import { ESPLoader, Transport } from "https://cdn.jsdelivr.net/npm/esptool-js@0.5.7/bundle.js";

const $ = (id) => document.getElementById(id);
const logEl = $("log");
const statusEl = $("status");
const connectBtn = $("connect");
const disconnectBtn = $("disconnect");
const flashBtn = $("flash-btn");
const barWrap = document.querySelector(".progress");
const bar = $("bar");
const versionSelect = $("version-select");

// CI publishes firmware.bin to a Release per version; list them for the dropdown.
const REPO = "RaemondBW/OpenTrailPaper";
let releases = []; // [{ tag, url }]

async function loadReleases() {
  if (!versionSelect) return;
  try {
    const res = await fetch(`https://api.github.com/repos/${REPO}/releases?per_page=40`);
    if (!res.ok) throw new Error("HTTP " + res.status);
    const data = await res.json();
    releases = data
      .map((r) => {
        const bin = (r.assets || []).find((a) => a.name === "firmware.bin");
        return bin ? { tag: r.tag_name, url: bin.browser_download_url } : null;
      })
      .filter(Boolean);
    versionSelect.innerHTML = "";
    if (releases.length === 0) {
      versionSelect.innerHTML = '<option value="">No CI releases available yet</option>';
      return;
    }
    releases.forEach((r, i) => {
      const o = document.createElement("option");
      o.value = String(i);
      o.textContent = r.tag + (i === 0 ? "  (latest)" : "");
      versionSelect.appendChild(o);
    });
  } catch (e) {
    versionSelect.innerHTML = '<option value="">Could not load releases</option>';
    log("Release list error: " + (e?.message || e));
  }
}

// Download a release's firmware.bin as a binary (latin-1) string for esptool-js.
async function fetchReleaseBin(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error("download HTTP " + res.status);
  const buf = new Uint8Array(await res.arrayBuffer());
  let s = "";
  const CHUNK = 0x8000;
  for (let i = 0; i < buf.length; i += CHUNK) {
    s += String.fromCharCode.apply(null, buf.subarray(i, i + CHUNK));
  }
  return s;
}

loadReleases();

let transport = null;
let esploader = null;
let device = null;

const term = {
  clean() { logEl.textContent = ""; },
  writeLine(data) { log(data); },
  write(data) { logEl.textContent += data; logEl.scrollTop = logEl.scrollHeight; },
};

function log(msg) {
  logEl.textContent += msg + "\n";
  logEl.scrollTop = logEl.scrollHeight;
}

function setStatus(text, kind) {
  statusEl.textContent = text;
  statusEl.className = "status" + (kind ? " " + kind : "");
}

// --- browser support gate ---------------------------------------------------
if (!("serial" in navigator)) {
  $("unsupported").hidden = false;
  connectBtn.disabled = true;
  setStatus("Web Serial not available in this browser.", "err");
}

// --- connect / disconnect ---------------------------------------------------
connectBtn.addEventListener("click", async () => {
  try {
    device = await navigator.serial.requestPort();
    transport = new Transport(device, true);
    esploader = new ESPLoader({
      transport,
      baudrate: 115200,
      terminal: term,
    });
    setStatus("Connecting… (board must be in download mode)");
    const chip = await esploader.main();
    setStatus(`Connected: ${chip}`, "ok");
    connectBtn.hidden = true;
    disconnectBtn.hidden = false;
    flashBtn.disabled = false;
  } catch (err) {
    console.error(err);
    log("Error: " + (err?.message || err));
    setStatus("Connection failed — is the board in download mode?", "err");
    await cleanup();
  }
});

disconnectBtn.addEventListener("click", cleanup);

async function cleanup() {
  try { if (transport) await transport.disconnect(); } catch (_) {}
  transport = null;
  esploader = null;
  device = null;
  connectBtn.hidden = false;
  disconnectBtn.hidden = true;
  flashBtn.disabled = true;
  barWrap.hidden = true;
  bar.style.width = "0";
  if (statusEl.className.indexOf("err") === -1) setStatus("Not connected.");
}

// --- flash ------------------------------------------------------------------
flashBtn.addEventListener("click", async () => {
  if (!esploader) return;

  // Only the official CI firmware is flashable, always at 0x10000 (the app
  // partition) — no custom binaries, offsets or partition edits.
  const fileArray = [];
  if (!versionSelect || versionSelect.value === "") {
    setStatus("Pick a firmware version to flash.", "err");
    return;
  }
  const rel = releases[parseInt(versionSelect.value, 10)];
  if (!rel) {
    setStatus("No firmware release available to flash.", "err");
    return;
  }
  try {
    setStatus(`Downloading firmware ${rel.tag}…`, "");
    const data = await fetchReleaseBin(rel.url);
    fileArray.push({ data, address: 0x10000, name: `firmware.bin (${rel.tag})` });
  } catch (e) {
    console.error(e);
    log("Download error: " + (e?.message || e));
    setStatus("Couldn't download the firmware (network/CORS). Try again.", "err");
    return;
  }

  flashBtn.disabled = true;
  connectBtn.disabled = true;
  barWrap.hidden = false;
  bar.style.width = "0";
  const numFiles = fileArray.length;

  try {
    for (const f of fileArray) {
      log(`Flashing ${f.name} → 0x${f.address.toString(16)} (${f.data.length} bytes)`);
    }
    setStatus("Flashing… do not unplug.", "");

    await esploader.writeFlash({
      fileArray,
      flashSize: "keep",
      flashMode: "keep",
      flashFreq: "keep",
      eraseAll: false,
      compress: true,
      // esptool-js reports written/size in compressed bytes per file, so track
      // the per-file fraction across the file count rather than summing bytes.
      reportProgress: (fileIndex, written, size) => {
        const frac = size > 0 ? written / size : 0;
        const pct = Math.min(100, Math.round(((fileIndex + frac) / numFiles) * 100));
        bar.style.width = pct + "%";
      },
    });

    bar.style.width = "100%";
    setStatus("Done! Tap RESET on the board to run the new firmware.", "ok");
    log("\n✔ Flash complete. Tap RESET to boot.");
    try { await esploader.after(); } catch (_) { /* OTG can't auto-reset; that's fine */ }
  } catch (err) {
    console.error(err);
    log("Error: " + (err?.message || err));
    setStatus("Flash failed — see the log above.", "err");
  } finally {
    flashBtn.disabled = false;
    connectBtn.disabled = false;
  }
});
