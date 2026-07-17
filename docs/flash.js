// Web Serial firmware flasher for the Open E-Paper Bike Computer.
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
const rowsEl = $("rows");
const barWrap = document.querySelector(".progress");
const bar = $("bar");

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

// --- file rows --------------------------------------------------------------
function refreshRemoveButtons() {
  const rows = rowsEl.querySelectorAll(".file-row");
  rows.forEach((r) => {
    r.querySelector("button.rm").hidden = rows.length <= 1;
  });
}

$("addrow").addEventListener("click", () => {
  const row = document.createElement("div");
  row.className = "file-row";
  row.innerHTML =
    '<input type="text" value="0x0" aria-label="Flash offset" class="offset">' +
    '<input type="file" accept=".bin" class="bin" aria-label="firmware binary">' +
    '<button class="rm" title="Remove row">×</button>';
  rowsEl.appendChild(row);
  refreshRemoveButtons();
});

rowsEl.addEventListener("click", (e) => {
  if (e.target.classList.contains("rm")) {
    e.target.closest(".file-row").remove();
    refreshRemoveButtons();
  }
});

// Read a File as a binary (latin-1) string, which is what esptool-js wants.
function readBinaryString(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(reader.result);
    reader.onerror = () => reject(reader.error);
    reader.readAsBinaryString(file);
  });
}

// Parse a flash offset, always interpreted as hexadecimal ("0x10000" or "10000").
function parseOffset(str) {
  const cleaned = String(str).trim().replace(/^0x/i, "");
  if (!/^[0-9a-f]+$/i.test(cleaned)) return NaN;
  return parseInt(cleaned, 16);
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

  // Gather and validate rows.
  const rows = [...rowsEl.querySelectorAll(".file-row")];
  const fileArray = [];
  for (const row of rows) {
    const fileInput = row.querySelector("input.bin");
    const file = fileInput.files[0];
    if (!file) continue; // skip empty rows
    const address = parseOffset(row.querySelector("input.offset").value);
    if (Number.isNaN(address)) {
      setStatus("Bad offset — use hex like 0x10000.", "err");
      return;
    }
    const data = await readBinaryString(file);
    fileArray.push({ data, address, name: file.name });
  }

  if (fileArray.length === 0) {
    setStatus("Choose at least one .bin file to flash.", "err");
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
      eraseAll: $("erase").checked,
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

refreshRemoveButtons();
