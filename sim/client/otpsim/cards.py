"""SD card contents for the sim: a rider's card as the firmware lays it out.

    from otpsim import cards
    card = cards.build("typical", "/tmp/cards/typical")   # once
    card.install(dev_dir)                                   # per device: a clone of it
    dev = SimDevice(dir=dev_dir, args=card.sim_args + [...])

Profiles (PROFILES):

    empty        nothing on the card
    typical      a season: ~150 rides, a dozen routes, a few workouts, a
                 city's map tiles, a couple of months of logs
    heavy        years of it: ~1000 rides, ~100 routes, ~5000 map tiles,
                 a year of big logs
    nearly_full  typical, on a card with only a few MB left

The layout is the firmware's own:

    /rides/YYYYMMDD-HHMMSS.fit      ride_recorder.cpp (FIT from fit_writer.cpp,
                                    written by `otp-sim card-tool fit`)
    /routes/<name>.gpx              routes.cpp
    /workouts/<name>.erg|.mrc       workout_service.cpp / workout.cpp
    /maps/tiles/<id[:6]>/<id[6:]>.ebm   map_store.cpp, res-6 H3 cell ids from
                                    `otp-sim card-tool h3` (the firmware's H3)
    /logs/YYYYMMDD.log              diag.cpp

Map tiles: every cell gets a small valid EBM2 tile made from the sample map
in data/sf.ebm, shaped to the cell's box (sample_tile). Real tiles can be
swapped in with set_tile_source(fn), fn(cell_id, (s, w, n, e)) -> bytes.

A built card has a manifest (Card.rides / routes / workouts / tiles / logs:
name -> size) for tests to compare against what the device reports."""
import json
import math
import os
import random
import shutil
import struct
import subprocess
import time

from .device import BINARY, ROOT

SAMPLE_MAP = os.path.join(ROOT, "data", "sf.ebm")

PROFILES = {
    "empty": dict(rides=0, routes=0, workouts=0, box=None, log_days=0, log_kb=0),
    "typical": dict(rides=150, routes=12, workouts=4,
                    box=(37.20, -122.60, 38.00, -121.80),          # the Bay Area: ~150 cells
                    log_days=60, log_kb=120, ride_from="2026-04-01", ride_days=200),
    "heavy": dict(rides=1000, routes=100, workouts=12,
                  box=(36.20, -123.70, 40.00, -118.80),            # much of northern California: ~5000 cells
                  log_days=365, log_kb=400, ride_from="2022-03-01", ride_days=1690),
    "nearly_full": dict(base="typical", card_mb=128, cluster_kb=4, free_mb=4),
}

_tile_source = None


def set_tile_source(fn):
    """Use fn(cell_id, (s, w, n, e)) -> bytes for every map tile (None: the
    built-in sample tile). Cards already built keep the tiles they have."""
    global _tile_source
    _tile_source = fn


# ---- map tiles ---------------------------------------------------------------------------------

_sample_subtiles = None


def _subtiles():
    """Non-empty sub-tile blobs of the sample map (ebm_info.py documents the
    format): u16 count, then per road u8 class, u16 n, n x (i16 x, i16 y) metres
    from the sub-tile's origin (+ an optional per-road flag trailer)."""
    global _sample_subtiles
    if _sample_subtiles is None:
        b = open(SAMPLE_MAP, "rb").read()
        assert b[:4] == b"EBM2"
        nx, ny = struct.unpack_from("<ii", b, 28)
        subs = []
        for k in range(nx * ny):
            off, ln = struct.unpack_from("<II", b, 36 + k * 8)
            if off and 1500 <= ln <= 12000:
                subs.append(b[off:off + ln])
        subs.sort(key=len)
        _sample_subtiles = subs
    return _sample_subtiles


def sample_tile(cell_id, box):
    """A valid EBM2 tile for `cell_id`: a 2 x 2 grid over the cell's box, each
    square holding roads borrowed from the sample map. ~10-30 KB, which is
    about what a real res-6 tile weighs."""
    s, w, n, e = box
    td = max(n - s, e - w) / 2.0
    subs = _subtiles()
    h = int(cell_id, 16)
    body = b""
    index = b""
    base = 36 + 4 * 8
    for i in range(4):
        blob = subs[(h + i * 7919) % len(subs)]
        index += struct.pack("<II", base + len(body), len(blob))
        body += blob
    return b"EBM2" + struct.pack("<dddii", s, w, td, 2, 2) + index + body


def tile_path(cell_id):
    """map_store.cpp tilePathForId: /maps/tiles/<first 6>/<rest>.ebm"""
    return "/maps/tiles/%s/%s.ebm" % (cell_id[:6], cell_id[6:])


def h3_cells(box):
    """[(cell_id, (s, w, n, e))] covering the box, from the firmware's own H3."""
    out = subprocess.run([BINARY, "card-tool", "h3"] + ["%.6f" % v for v in box], check=True,
                         capture_output=True, text=True).stdout
    cells = []
    for line in out.splitlines():
        p = line.split()
        cells.append((p[0], tuple(float(v) for v in p[1:5])))
    return cells


# ---- routes, workouts, logs ---------------------------------------------------------------------

GPX = '<?xml version="1.0"?>\n<gpx version="1.1" creator="otpsim"><trk><name>%s</name><trkseg>\n%s\n</trkseg></trk></gpx>\n'


def gpx_route(name, lat, lon, km, rng, step_m=25):
    """A wandering out-and-about track of `km`, a point every `step_m`."""
    pts = []
    heading = rng.uniform(0, 2 * math.pi)
    for i in range(int(km * 1000 / step_m) + 1):
        heading += rng.uniform(-0.15, 0.15)
        lat += step_m * math.cos(heading) / 111320.0
        lon += step_m * math.sin(heading) / (111320.0 * math.cos(math.radians(lat)))
        pts.append('<trkpt lat="%.6f" lon="%.6f"><ele>%.1f</ele></trkpt>' % (lat, lon, 30 + 20 * math.sin(i / 50.0)))
    return (GPX % (name, "\n".join(pts))).encode()


def erg_workout(name, rng):
    lines = ["[COURSE HEADER]", "VERSION = 2", "UNITS = ENGLISH", "DESCRIPTION = %s" % name,
             "FILE NAME = %s" % name, "MINUTES WATTS", "[END COURSE HEADER]", "[COURSE DATA]"]
    t = 0.0
    watts = 120
    for _ in range(rng.randint(6, 16)):
        lines.append("%.2f\t%d" % (t, watts))
        t += rng.choice([2, 3, 5, 8, 10])
        lines.append("%.2f\t%d" % (t, watts))
        watts = rng.choice([120, 150, 200, 250, 280, 320])
    lines.append("[END COURSE DATA]")
    return ("\n".join(lines) + "\n").encode()


LOG_LINES = [
    "gps: fix 3D sats=%d hdop=0.9",
    "rec: auto-pause (5 s stopped) at %d.42 km",
    "rec: auto-resume (paused %ds)",
    "srv: phone connected, mtu 2%d",
    "link: hello ios 1.%d",
    "map: viewport needs %d tiles, budget is 24",
    "ble: hr strap 150 bpm rssi -%d",
    "power: battery %d%%",
]


def log_text(kb, rng):
    out = []
    size = 0
    t = 6 * 3600
    while size < kb * 1024:
        t += rng.randint(1, 30)
        line = "%02d:%02d:%02d %s" % (t // 3600 % 24, t // 60 % 60, t % 60,
                                      rng.choice(LOG_LINES) % rng.randint(1, 99))
        out.append(line)
        size += len(line) + 1
    return ("\n".join(out) + "\n").encode()


# ---- the card -------------------------------------------------------------------------------------

def _clusters(nbytes, cluster):
    return (nbytes + cluster - 1) // cluster


def card_usage(sd, card_mb, cluster_kb=None):
    """(total, used) bytes as the sim's card (fs_sim.cpp) counts them."""
    size = card_mb << 20
    cluster = (cluster_kb or (32 if size >= (2 << 30) else 16)) * 1024
    total_clusters = (size - 32 * 512) // (cluster + 8)

    def tree(p):
        n = 0
        dir_bytes = 0
        with os.scandir(p) as it:
            for e in it:
                dir_bytes += 32 * (1 + (len(e.name) + 12) // 13)
                if e.is_dir(follow_symlinks=False):
                    n += tree(e.path)
                else:
                    n += _clusters(e.stat().st_size, cluster)
        return n + max(1, _clusters(dir_bytes, cluster))

    return total_clusters * cluster, min(tree(sd), total_clusters) * cluster


class Card:
    """A built card image: <path>/sd plus its manifest."""

    def __init__(self, path):
        self.path = path
        self.sd = os.path.join(path, "sd")
        with open(os.path.join(path, "manifest.json")) as f:
            m = json.load(f)
        self.profile = m["profile"]
        self.rides = m["rides"]
        self.routes = m["routes"]
        self.workouts = m["workouts"]
        self.tiles = m["tiles"]
        self.logs = m["logs"]
        self.card_mb = m.get("card_mb")
        self.cluster_kb = m.get("cluster_kb")
        self.build_s = m.get("build_s", 0)

    @property
    def sim_args(self):
        args = ["--sd-size-mb", str(self.card_mb)] if self.card_mb else []
        return args + (["--sd-cluster-kb", str(self.cluster_kb)] if self.cluster_kb else [])

    def install(self, dev_dir):
        """Put a copy of the card in a device directory (<dev_dir>/sd). On
        APFS it is a copy-on-write clone: instant, and the device's writes
        never reach the cached image."""
        dst = os.path.join(dev_dir, "sd")
        if os.path.exists(dst):
            shutil.rmtree(dst)
        os.makedirs(dev_dir, exist_ok=True)
        if subprocess.run(["/bin/cp", "-cR", self.sd, dst], capture_output=True).returncode != 0:
            shutil.copytree(self.sd, dst)
        return dst


def build(profile, path, seed=1):
    """Fill <path>/sd with `profile` (PROFILES) and write its manifest.
    Returns the Card. Rebuilds from scratch."""
    spec = dict(PROFILES[profile])
    t0 = time.time()
    if os.path.exists(path):
        shutil.rmtree(path)
    sd = os.path.join(path, "sd")
    os.makedirs(sd)
    card_mb = cluster_kb = None
    filler = None
    if "base" in spec:
        base = spec.pop("base")
        card_mb = spec["card_mb"]
        cluster_kb = spec.get("cluster_kb")
        spec = dict(PROFILES[base], **spec)
    rng = random.Random("%s:%d" % (profile, seed))
    man = dict(profile=profile, rides={}, routes={}, workouts={}, tiles={}, logs={}, card_mb=card_mb,
               cluster_kb=cluster_kb)

    def put(card_path, data):
        host = sd + card_path
        os.makedirs(os.path.dirname(host), exist_ok=True)
        with open(host, "wb") as f:
            f.write(data)
        return len(data)

    # Rides, through the firmware's FitWriter.
    if spec["rides"]:
        y, mo, d = (int(v) for v in spec["ride_from"].split("-"))
        first = int(time.mktime((y, mo, d, 0, 0, 0, 0, 0, -1)))
        days = sorted(rng.sample(range(spec["ride_days"]), spec["rides"]))
        specs = []
        for day in days:
            start = first + day * 86400 + rng.choice([6, 7, 8, 9, 16, 17, 18]) * 3600 + rng.randint(0, 3599)
            secs = int(rng.triangular(25 * 60, 4 * 3600, 70 * 60))
            name = time.strftime("%Y%m%d-%H%M%S", time.gmtime(start)) + ".fit"
            specs.append("/rides/%s %d %d %.5f %.5f %d %.1f %d %d" % (
                name, start, secs, 37.6 + rng.uniform(0, 0.3), -122.5 + rng.uniform(0, 0.3),
                rng.randint(0, 359), rng.uniform(18, 32), rng.choice([0, 135, 145, 155]),
                rng.choice([0, 0, 180, 220])))
        r = subprocess.run([BINARY, "card-tool", "fit", sd], input="\n".join(specs) + "\n",
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError("card-tool fit: " + r.stderr)
        for name in os.listdir(os.path.join(sd, "rides")):
            man["rides"][name] = os.path.getsize(os.path.join(sd, "rides", name))

    for i in range(spec["routes"]):
        name = "%s %02d.gpx" % (rng.choice(["Loop", "Coast", "Hills", "Commute", "Gravel", "Ridge"]), i)
        man["routes"][name] = put("/routes/" + name, gpx_route(name[:-4], 37.77, -122.42,
                                                                rng.uniform(15, 60), rng))
    for i in range(spec["workouts"]):
        name = "%s%d.erg" % (rng.choice(["sweetspot", "vo2", "endurance", "threshold"]), i)
        man["workouts"][name] = put("/workouts/" + name, erg_workout(name, rng))

    if spec["box"]:
        src = _tile_source or sample_tile
        for cell, box in h3_cells(spec["box"]):
            man["tiles"][cell] = put(tile_path(cell), src(cell, box))

    for i in range(spec["log_days"]):
        day = time.gmtime(time.time() - (i + 2) * 86400)   # not today: the device writes that one
        name = time.strftime("%Y%m%d", day) + ".log"
        man["logs"][name] = put("/logs/" + name, log_text(rng.randint(spec["log_kb"] // 3, spec["log_kb"]), rng))

    if card_mb:
        # Whatever else is on the rider's card (photos, an old backup):
        # one sparse file, sized so `free_mb` is left.
        total, used = card_usage(sd, card_mb, cluster_kb)
        filler = total - used - (spec["free_mb"] << 20) - (64 << 10)
        if filler <= 0:
            raise RuntimeError("%s: the content alone fills a %d MB card" % (profile, card_mb))
        with open(os.path.join(sd, "backup.img"), "wb") as f:
            f.truncate(filler)
        man["filler"] = filler

    man["build_s"] = round(time.time() - t0, 2)
    with open(os.path.join(path, "manifest.json"), "w") as f:
        json.dump(man, f)
    return Card(path)


def load(path):
    return Card(path)
