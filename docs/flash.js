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

// CI publishes firmware.bin per version; the Pages build bundles them into
// firmware/ alongside this site (see loadReleases).
let releases = []; // [{ tag, url }]

// Firmware is served from THIS site (firmware/<tag>/firmware.bin), not from the
// GitHub Release asset URL.
//
// GitHub moved release-asset downloads to release-assets.githubusercontent.com
// (Azure Blob), which sends no access-control-allow-origin header, so a browser
// fetch() of one fails with "Failed to fetch". The api.github.com asset endpoint
// does not help either: its redirect carries CORS but the blob it points at does
// not, and the browser needs CORS on the final response. The Pages workflow
// downloads the binaries server-side and ships them alongside the site, so this
// is a same-origin fetch with no CORS involved.
async function loadReleases() {
  if (!versionSelect) return;
  try {
    const res = await fetch("firmware/index.json", { cache: "no-cache" });
    if (!res.ok) throw new Error("HTTP " + res.status);
    releases = await res.json();
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

// --- leaving download mode ---------------------------------------------------
// soc/rtc_cntl_reg.h: DR_REG_RTCCNTL_BASE (0x60008000) + 0x12C, bit 0.
const RTC_CNTL_OPTION1_REG = 0x6000812c;
const RTC_CNTL_FORCE_DOWNLOAD_BOOT = 0x1;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Reset the board so it runs the app instead of returning to download mode.
// Two things stand in the way, and both are handled here:
//
//  1. Whatever put the board into download mode set FORCE_DOWNLOAD_BOOT. That
//     bit lives in the RTC domain, which a reset does not clear, so the ROM
//     would divert to the loader again. We clear it over the connection that
//     just did the flashing, while the loader is still listening.
//
//  2. On the USB-Serial-JTAG bridge, DTR drives GPIO0 and RTS drives EN — and
//     Transport.setRTS() re-asserts the last DTR state on every call (a
//     Windows driver workaround). Deassert DTR *first*, or every RTS pulse
//     also pulls GPIO0 low and boots the loader.
async function bootIntoApp() {
  await esploader.writeReg(
    RTC_CNTL_OPTION1_REG,
    0x0,
    RTC_CNTL_FORCE_DOWNLOAD_BOOT, // mask: leave the rest of the register alone
  );
  log("Cleared the force-download-boot bit.");

  await transport.setDTR(false); // GPIO0 high — boot from flash
  await transport.setRTS(false); // idle
  await sleep(100);
  await transport.setRTS(true); // EN low: chip in reset
  await sleep(100);
  await transport.setRTS(false); // EN high: boots, sampling GPIO0 high
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
    log("\n✔ Flash complete.");

    // Boot the new firmware without a RESET tap. esptool's own after() can't:
    // it pulses RTS while leaving DTR asserted, and on this board's
    // USB-Serial-JTAG bridge DTR drives GPIO0 — so its reset lands right back
    // in download mode. See bootIntoApp() for the sequence that works.
    let resetErr = null;
    try {
      await bootIntoApp();
      log("Reset sent — the board should be running the new firmware now.");
    } catch (e) {
      console.error(e);
      resetErr = e;
      log("Couldn't reset automatically (" + (e?.message || e) + "). Tap RESET to boot.");
    }

    // The board re-enumerates as a different USB device on the way out of
    // download mode, so this port is dead either way — drop it and reset the UI
    // rather than leaving a Disconnect button pointing at nothing.
    await cleanup();
    setStatus(
      resetErr
        ? "Flashed OK, but the reset failed — tap RESET on the board."
        : "Done! The board is booting the new firmware.",
      "ok",
    );
  } catch (err) {
    console.error(err);
    log("Error: " + (err?.message || err));
    setStatus("Flash failed — see the log above.", "err");
  } finally {
    // Only offer Flash again if we still hold a connection: on the success path
    // cleanup() has already dropped it, and the board is off running the app.
    flashBtn.disabled = esploader === null;
    connectBtn.disabled = false;
  }
});
