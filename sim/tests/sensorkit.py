"""Helpers shared by the sensor, radar and media tests: pairing a virtual
sensor the way the app's Sensors sheet does, riding, reading the glass."""
import math
import os
import struct
import time
import zlib

from conftest import TIME_SCALE
from otpsim import pb

W, H = 540, 960
SAVE_BUTTON = (24 + 120, 700 + 48)   # ui_render.cpp kSaveButton centre


def scan_until_listed(link, addr, timeout=10):
    link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_SCAN_START))
    return link.wait_push("sensor_list", lambda l: any(s.address == addr for s in l.sensors),
                          timeout=timeout * TIME_SCALE)


def pair_sensor(link, addr):
    """As the app's Sensors sheet does it: scan, see it, pair it, stop."""
    scan_until_listed(link, addr)
    r = link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_PAIR, address=addr))
    link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_SCAN_STOP))
    assert any(s.address == addr and s.paired for s in r.sensor_list.sensors), r
    return r.sensor_list


def add_and_pair(device, link, kind, **values):
    addr = device.sensor(kind, **values)
    pair_sensor(link, addr)
    return addr


def sensor_list(link):
    return link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_LIST)).sensor_list


def listed(link, addr):
    for s in sensor_list(link).sensors:
        if s.address == addr:
            return s
    return None


def eventually(fn, predicate, message, timeout=10, interval=0.1):
    """Poll fn() until predicate(value); the assertion names what never happened."""
    deadline = time.time() + timeout * TIME_SCALE
    last = None
    while time.time() < deadline:
        last = fn()
        if predicate(last):
            return last
        time.sleep(interval)
    raise AssertionError("%s (last: %s)" % (message, str(last)[:400]))


def holds(fn, predicate, message, seconds, interval=0.1):
    """fn() satisfies predicate for the whole of `seconds` (wall clock)."""
    end = time.time() + seconds
    while time.time() < end:
        v = fn()
        assert predicate(v), "%s (%s)" % (message, str(v)[:400])
        time.sleep(interval)


def state(device):
    return device.sensor_state()["state"]


def links(device):
    return device.sensor_state()["links"]


# -- riding -----------------------------------------------------------------------------------

def line_track(lat, lon, km, bearing_deg=90, step_m=20):
    pts = []
    r = math.radians(bearing_deg)
    for i in range(int(km * 1000 / step_m) + 1):
        d = i * step_m
        pts.append((lat + d * math.cos(r) / 111320.0,
                    lon + d * math.sin(r) / (111320.0 * math.cos(math.radians(lat)))))
    return pts


def start_ride(device, gps=True, speed_kmh=30):
    if gps:
        device.gps_track(line_track(37.77, -122.45, 10), speed_kmh=speed_kmh)
        device.wait_status(lambda s: s["ride"]["gps_fix"] and s["ride"]["time_valid"], timeout=15)
    device.button("boot")
    device.wait_status(lambda s: s["ride"]["recording"], timeout=10)


def save_ride(device):
    """Stop, SAVE on the summary, and return the finished FIT's bytes."""
    rides = os.path.join(device.sd, "rides")
    before = set(os.listdir(rides)) if os.path.isdir(rides) else set()
    device.button("boot")
    device.wait_status(lambda s: s["screen"] == "other", timeout=10)
    device.tap(*SAVE_BUTTON)
    device.wait_status(lambda s: not s["ride"]["recording"], timeout=20)
    deadline = time.time() + 10
    while time.time() < deadline:
        names = [n for n in os.listdir(rides) if n.endswith(".fit")] if os.path.isdir(rides) else []
        if names:
            newest = max(names, key=lambda n: os.path.getmtime(os.path.join(rides, n)))
            data = open(os.path.join(rides, newest), "rb").read()
            hdr = data[0] if data else 0
            if len(data) > 14 and len(data) == hdr + struct.unpack_from("<I", data, 4)[0] + 2:
                return data
        time.sleep(0.2)
    raise AssertionError("no finished ride on the card")


# -- the glass ---------------------------------------------------------------------------------

class Glass:
    """A screenshot as rows of 0-255 grey levels."""

    def __init__(self, path):
        data = open(path, "rb").read()
        pos, idat = 8, b""
        while pos < len(data):
            n, kind = struct.unpack_from(">I4s", data, pos)
            body = data[pos + 8:pos + 8 + n]
            if kind == b"IHDR":
                self.w, self.h = struct.unpack_from(">II", body)
            elif kind == b"IDAT":
                idat += body
            pos += 12 + n
        raw = zlib.decompress(idat)
        stride = self.w + 1
        self.rows = [raw[y * stride + 1:(y + 1) * stride] for y in range(self.h)]

    def region(self, x0, y0, x1, y1):
        return bytes(b for y in range(y0, y1) for b in self.rows[y][x0:x1])

    def ink(self, x0, y0, x1, y1, level=0x40):
        """Fraction of pixels in the box at or darker than `level`."""
        r = self.region(x0, y0, x1, y1)
        return sum(1 for b in r if b <= level) / max(len(r), 1)

    def dots(self, x0, y0, x1, y1):
        """How strongly the box carries the greyed-out cell's TONE_25 dot."""
        on = off = n_on = n_off = 0
        for y in range(y0, y1):
            row = self.rows[y]
            for x in range(x0, x1):
                dark = row[x] <= 0x40
                if not (x & 1) and not (y & 1):
                    n_on += 1
                    on += dark
                else:
                    n_off += 1
                    off += dark
        return on / max(n_on, 1) - off / max(n_off, 1)


def glass(device, tmp_path, name):
    path = str(tmp_path / (name + ".png"))
    device.screenshot(path)
    return Glass(path)


def settle(device, quiet_s=1.5, timeout=10):
    """Wait until the panel has gone `quiet_s` without a paint."""
    deadline = time.time() + timeout * TIME_SCALE
    last, since = device.status()["paints"], time.time()
    while time.time() < deadline:
        time.sleep(0.2)
        p = device.status()["paints"]
        if p != last:
            last, since = p, time.time()
        elif time.time() - since >= quiet_s:
            return device.status()
    raise AssertionError("the panel never settled")


def set_dash(link, text):
    r = link.request(set_dash=pb.DashConfig(text=text))
    assert r.WhichOneof("body") == "dash", r
    return r.dash.text
