"""Helpers for the power and robustness tests (test_power.py,
test_crash_recovery.py, test_gps_edges.py): running the device clock to a
moment while watching for it to power itself off, and reading FIT records."""
import os
import struct
import time

from conftest import TIME_SCALE
from otpsim.device import ControlError

FAST = ["--gps-baud", "115200"]
IDLE_S = 600            # ui_dashboard.cpp AUTO_SLEEP_MS
FAST_SPEED = 60         # device seconds per real second while nothing is due
FINE_SPEED = 10         # ... and near the moment that matters


def uptime_s(dev):
    return dev.status()["uptime_ms"] / 1000.0


def run_to(dev, until_s, fine_from=None):
    """Run the device clock fast until its uptime reaches `until_s` s.

    Returns None if the device is still on then (the clock goes back to 1x),
    else the last uptime seen before it powered itself off. From `fine_from`
    on the clock runs at FINE_SPEED, so that moment is resolved to ~1 s of
    device time."""
    last = uptime_s(dev)
    fine = fine_from is not None and last >= fine_from
    dev.set_speed(FINE_SPEED if fine else FAST_SPEED)
    deadline = time.time() + ((until_s - last) / FINE_SPEED + 30) * TIME_SCALE
    while time.time() < deadline:
        try:
            st = dev.status()
        except (ControlError, OSError, ValueError):
            if not dev.running or _exits(dev):
                dev.wait_powered_off(5)
                return last
            raise
        last = st["uptime_ms"] / 1000.0
        if last >= until_s:
            dev.set_speed(1)
            return None
        if not fine and fine_from is not None and last >= fine_from:
            fine = True
            dev.set_speed(FINE_SPEED)
        time.sleep(0.05)
    raise TimeoutError("the device clock never reached %.0f s (at %.0f s)" % (until_s, last))


def _exits(dev, wait=3.0):
    try:
        dev.proc.wait(timeout=wait)
        return True
    except Exception:
        return False


def console_text(dev):
    with open(dev.log_path, "r", errors="replace") as f:
        return f.read()


def card_logs(dev):
    """Everything in the card's /logs/*.log, concatenated."""
    d = os.path.join(dev.sd, "logs")
    out = ""
    for n in sorted(os.listdir(d)) if os.path.isdir(d) else []:
        if n.endswith(".log"):
            out += open(os.path.join(d, n), errors="replace").read()
    return out


# ---- FIT --------------------------------------------------------------------------------

FIT_EPOCH = 631065600   # 1989-12-31T00:00:00Z


def fit_crc(data):
    table = [0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
             0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400]
    crc = 0
    for b in data:
        tmp = table[crc & 0xF]
        crc = (crc >> 4) & 0x0FFF
        crc = crc ^ tmp ^ table[b & 0xF]
        tmp = table[crc & 0xF]
        crc = (crc >> 4) & 0x0FFF
        crc = crc ^ tmp ^ table[(b >> 4) & 0xF]
    return crc


def check_finished_fit(data):
    """A finished activity file: header, its declared data size, the file CRC."""
    assert len(data) > 14, len(data)
    hdr = data[0]
    assert hdr in (12, 14) and data[8:12] == b".FIT", data[:14]
    size = struct.unpack_from("<I", data, 4)[0]
    assert len(data) == hdr + size + 2, "declared %d data bytes, file has %d" % (size, len(data) - hdr - 2)
    stored = struct.unpack("<H", data[-2:])[0]
    assert stored == fit_crc(data[:-2]), "file CRC 0x%04x, contents 0x%04x" % (stored, fit_crc(data[:-2]))


def fit_messages(data):
    """[(global message number, {field number: raw value})] for every data
    message of a FIT file (no compressed timestamps, no developer fields beyond
    skipping them: what FitWriter writes)."""
    hdr = data[0]
    size = struct.unpack_from("<I", data, 4)[0]
    pos, end = hdr, hdr + size
    defs = {}
    out = []
    fmt = {1: "b", 2: "B", 0x83: "h", 0x84: "H", 0x85: "i", 0x86: "I", 0x88: "f", 0x89: "d",
           0x8B: "H", 0x8C: "I", 0x0A: "B", 0x0D: "B", 0x00: "B", 0x07: None}
    while pos < end:
        rh = data[pos]
        pos += 1
        assert not rh & 0x80, "compressed timestamp header at %d" % (pos - 1)
        local = rh & 0x0F
        if rh & 0x40:
            arch = data[pos + 1]
            gnum = struct.unpack_from(">H" if arch else "<H", data, pos + 2)[0]
            n = data[pos + 4]
            fields = [tuple(data[pos + 5 + 3 * i:pos + 8 + 3 * i]) for i in range(n)]
            pos += 5 + 3 * n
            dev = []
            if rh & 0x20:
                nd = data[pos]
                dev = [tuple(data[pos + 1 + 3 * i:pos + 4 + 3 * i]) for i in range(nd)]
                pos += 1 + 3 * nd
            defs[local] = (gnum, arch, fields, sum(f[1] for f in dev))
        else:
            gnum, arch, fields, devsize = defs[local]
            vals = {}
            for num, sz, base in fields:
                raw = data[pos:pos + sz]
                pos += sz
                f = fmt.get(base)
                if f and struct.calcsize(f) == sz:
                    vals[num] = struct.unpack((">" if arch else "<") + f, raw)[0]
                else:
                    vals[num] = raw
            pos += devsize
            out.append((gnum, vals))
    return out


def fit_records(data):
    """The ride's record messages (global 20) as dicts: utc, lat, lon, distance_m."""
    recs = []
    for gnum, v in fit_messages(data):
        if gnum != 20:
            continue
        lat = v.get(0)
        lon = v.get(1)
        recs.append({
            "utc": v[253] + FIT_EPOCH if 253 in v else None,
            "lat": lat * 180.0 / 2 ** 31 if isinstance(lat, int) and lat != 0x7FFFFFFF else None,
            "lon": lon * 180.0 / 2 ** 31 if isinstance(lon, int) and lon != 0x7FFFFFFF else None,
            "distance_m": v[5] / 100.0 if isinstance(v.get(5), int) and v[5] != 0xFFFFFFFF else None,
        })
    return recs


def only_ride(dev):
    rides = sorted(n for n in os.listdir(os.path.join(dev.sd, "rides")) if n.endswith(".fit"))
    assert len(rides) == 1, rides
    return rides[0], open(os.path.join(dev.sd, "rides", rides[0]), "rb").read()
