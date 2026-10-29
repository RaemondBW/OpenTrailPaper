"""Real map tiles for tests: the committed fixture (sim/fixtures/tiles/, built
by its build_fixture.py from the prebuilt-tile CDN), a reader for the .ebm
format that projects roads the way src/map_tiles.cpp does, and the card
layout the firmware stores tiles in.

    from otpsim.tiles import fixture_tiles, fixture_manifest
    for h3, data, version in fixture_tiles():          # the .ebm files
        link.upload(pb.TARGET_MAP_TILE, h3, data, tile_version=version)

The tiles are OpenStreetMap data (ODbL): see sim/fixtures/tiles/README.md.
"""
import json
import math
import os
import struct
import zlib

FIXTURE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "fixtures", "tiles"))

# MapFeatureClass (src/map_view.h)
MAJOR, PRIMARY, SECONDARY, TERTIARY, MINOR, PATH = range(6)

# MapPoiType and MAP_PF_* flags (src/map_view.h, docs/mapgen.js POI_*)
WATER, TOILETS, REPAIR, BIKE_SHOP = 1, 2, 3, 4
PF_RESTRICTED, PF_PUMP = 0x80, 0x01

# The map page's geometry (src/ui_dashboard.cpp buildMapScreenData, ui_render.h)
RIDER_X, RIDER_Y = 270, 430
MAP_TOP, MAP_BOTTOM = 64, 810
DEFAULT_MPP = 2.0


def fixture_manifest(fixture_dir=FIXTURE_DIR):
    """manifest.json: name, centre {lat, lon, h3}, bbox {s, w, n, e}, and per
    cell its sizes, hashes, version records ("cdn:<hash>") and bbox."""
    with open(os.path.join(fixture_dir, "manifest.json")) as f:
        return json.load(f)


def fixture_tiles(fixture_dir=FIXTURE_DIR):
    """[(h3, ebm bytes, version record)] for every cell, sorted by id."""
    m = fixture_manifest(fixture_dir)
    out = []
    for h3 in sorted(m["cells"]):
        with open(os.path.join(fixture_dir, h3 + ".ebm"), "rb") as f:
            out.append((h3, f.read(), m["cells"][h3]["ebm_version"]))
    return out


def fixture_pois(fixture_dir=FIXTURE_DIR):
    """[(h3, poi bytes, version record)] for the cells that have POIs."""
    m = fixture_manifest(fixture_dir)
    out = []
    for h3 in sorted(m["cells"]):
        c = m["cells"][h3]
        if c.get("poi_size"):
            with open(os.path.join(fixture_dir, h3 + ".poi"), "rb") as f:
                out.append((h3, f.read(), c["poi_version"]))
    return out


POI_DIR = os.path.join(FIXTURE_DIR, "pois")


def poi_set(name, poi_dir=POI_DIR):
    """A synthetic POI set (sim/fixtures/tiles/pois/, built by
    build_pois.mjs with docs/mapgen.js buildPoi): (meta, [(h3, poi bytes,
    version record)]). meta: `stand` {lat, lon} (where to open the map),
    `cells`, and `pois` [{name, kind, type, flags, lat, lon, cell}] (type 0:
    in the input only, the builder drops it)."""
    with open(os.path.join(poi_dir, "pois.json")) as f:
        meta = json.load(f)["sets"][name]
    out = []
    for h3 in sorted(meta["cells"]):
        with open(os.path.join(poi_dir, meta["cells"][h3]["file"]), "rb") as f:
            data = f.read()
        out.append((h3, data, "phone:%d" % (zlib.crc32(data) & 0x7FFFFFFF)))
    return meta, out


def card_path(sd_root, h3, ext=".ebm"):
    """Where the firmware keeps a cell on the card (map_store.cpp tilePathFor):
    /maps/tiles/<first 6 chars>/<rest><ext>."""
    return os.path.join(sd_root, "maps", "tiles", h3[:6], h3[6:] + ext)


def install_on_card(sd_root, tiles, ext=".ebm"):
    """Write tiles straight onto a card directory, as a computer would over
    USB (no /maps/tiles.ver record: the device lists them without a version).
    `tiles`: [(h3, bytes, ...)], e.g. fixture_tiles()."""
    for t in tiles:
        p = card_path(sd_root, t[0], ext)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(t[1])


# -- reading .ebm ----------------------------------------------------------------------

class Ebm:
    """One EBM2 blob: a grid of sub-tiles of road polylines (+ ELV1/WTR2/PRK2,
    not decoded here). Layout as src/map_tiles.cpp projectBlobInto reads it."""

    def __init__(self, data):
        if len(data) < 36 or data[:4] != b"EBM2":
            raise ValueError("not an EBM2 tile")
        self.data = data
        self.lat0, self.lon0, self.deg = struct.unpack_from("<ddd", data, 4)
        self.nx, self.ny = struct.unpack_from("<ii", data, 28)
        mid = self.lat0 + self.deg * self.ny / 2.0
        self.kx = 111320.0 * math.cos(math.radians(mid))
        self.ky = 110540.0

    def roads(self):
        """Every road as (cls, [(lat, lon), ...])."""
        b = self.data
        tw, th = self.deg * self.kx, self.deg * self.ky
        for ty in range(self.ny):
            for tx in range(self.nx):
                off, ln = struct.unpack_from("<II", b, 36 + (ty * self.nx + tx) * 8)
                if not off or off + ln > len(b):
                    continue
                p, end = off + 2, off + ln
                (count,) = struct.unpack_from("<H", b, off)
                for _ in range(count):
                    if p + 3 > end:
                        break
                    cls, n = b[p], struct.unpack_from("<H", b, p + 1)[0]
                    p += 3
                    if p + n * 4 > end:
                        break
                    pts = []
                    for j in range(n):
                        mx, my = struct.unpack_from("<hh", b, p + j * 4)
                        pts.append((self.lat0 + (ty * th + my) / self.ky,
                                    self.lon0 + (tx * tw + mx) / self.kx))
                    p += n * 4
                    yield cls, pts


def to_screen(lat, lon, centre_lat, centre_lon, mpp=DEFAULT_MPP, cx=RIDER_X, cy=RIDER_Y):
    """North-up projection of a point onto the map page (map_tiles geoToScreen,
    near enough: a pixel or so across one screen)."""
    kx = 111320.0 * math.cos(math.radians(centre_lat))
    return (cx + (lon - centre_lon) * kx / mpp, cy - (lat - centre_lat) * 110540.0 / mpp)


def screen_roads(blobs, lat, lon, mpp=DEFAULT_MPP, classes=(MAJOR, PRIMARY, SECONDARY, TERTIARY, MINOR)):
    """Roads of `blobs` (bytes) projected around (lat, lon) as
    (cls, [(x, y), ...]) in panel px, those with a point on the panel only."""
    out = []
    for blob in blobs:
        for cls, pts in Ebm(blob).roads():
            if cls not in classes:
                continue
            xy = [to_screen(a, o, lat, lon, mpp) for a, o in pts]
            if any(-50 < x < 590 and -50 < y < 1010 for x, y in xy):
                out.append((cls, xy))
    return out


def samples_along(xy, step=6.0):
    """Points every `step` px along a polyline."""
    out = []
    for (x0, y0), (x1, y1) in zip(xy, xy[1:]):
        d = math.hypot(x1 - x0, y1 - y0)
        k = max(1, int(d / step))
        for i in range(k):
            t = i / k
            out.append((x0 + (x1 - x0) * t, y0 + (y1 - y0) * t))
    if xy:
        out.append(xy[-1])
    return out


def keep_roads(data, keep):
    """A copy of an .ebm with only the roads whose class is in `keep`: what a
    newer version of a tile looks like when streets were removed upstream.
    The sections after the road data (ELV1, WTR2, PRK2, ...) are kept, moved
    up to follow the shorter road data as the reader expects."""
    e = Ebm(data)
    n = e.nx * e.ny
    base = 36 + n * 8
    old_end = base
    subs = []
    for k in range(n):
        off, ln = struct.unpack_from("<II", data, 36 + k * 8)
        if not off:
            subs.append(None)
            continue
        old_end = max(old_end, off + ln)
        end = off + ln
        (count,) = struct.unpack_from("<H", data, off)
        p, recs = off + 2, []
        for _ in range(count):
            m = struct.unpack_from("<H", data, p + 1)[0]
            recs.append(data[p:p + 3 + m * 4])
            p += 3 + m * 4
        flags = data[p:end] if end - p == count and count else None
        kept = [(r, flags[i] if flags else None) for i, r in enumerate(recs) if r[0] in keep]
        body = struct.pack("<H", len(kept)) + b"".join(r for r, _ in kept)
        if flags:
            body += bytes(f for _, f in kept)
        subs.append(body)
    out = bytearray(data[:base])
    for k, body in enumerate(subs):
        if body is None:
            continue
        struct.pack_into("<II", out, 36 + k * 8, len(out), len(body))
        out += body
    return bytes(out) + data[old_end:]


# -- .poi files --------------------------------------------------------------------------
# The format of src/map_tiles.cpp (reader) and docs/mapgen.js buildPoi (writer),
# little-endian: 'EPOI', u8 version (1), u8 recordSize (6), u16 count, u64 H3
# cell, f64 lat0, f64 lon0, f32 kx, f32 ky (40 bytes), then count records of
# u8 type, u8 flags, i16 x, i16 y (metres E/N of lat0/lon0), padded to
# recordSize.
POI_HEADER = struct.Struct("<4sBBHQddff")


def read_poi(data):
    """A .poi as a dict: version, rec_size, count, cell, lat0, lon0, kx, ky,
    and records [(type, flags, x, y)] (as many as the file holds)."""
    magic, ver, rs, count, cell, lat0, lon0, kx, ky = POI_HEADER.unpack_from(data)
    recs = []
    for i in range(count):
        o = POI_HEADER.size + i * max(rs, 6)
        if o + 6 > len(data):
            break
        recs.append(struct.unpack_from("<BBhh", data, o))
    return dict(magic=magic, version=ver, rec_size=rs, count=count, cell=cell, lat0=lat0, lon0=lon0,
                kx=kx, ky=ky, records=recs)


def write_poi(lat0, lon0, records, cell=0, kx=None, ky=110540.0, version=1, rec_size=6, count=None,
              magic=b"EPOI"):
    """Encode a .poi as buildPoi does — or, with the keyword arguments, as it
    must not: another version, a longer record (zero padded), a count that is
    not the number of records. records: [(type, flags, x, y)]; or use
    poi_record() for a lat/lon."""
    if kx is None:
        kx = 111320.0 * math.cos(math.radians(lat0))
    n = len(records) if count is None else count
    out = bytearray(POI_HEADER.pack(magic, version, rec_size, n, cell, lat0, lon0, kx, ky))
    for t, f, x, y in records:
        out += struct.pack("<BBhh", t, f, x, y) + bytes(max(rec_size - 6, 0))
    return bytes(out)


def poi_record(poi, ptype, flags, lat, lon):
    """(type, flags, x, y) for a POI at lat/lon in the frame of `poi` (a
    read_poi() dict), rounded as buildPoi rounds."""
    kx, ky = struct.unpack("<ff", struct.pack("<ff", poi["kx"], poi["ky"]))
    return (ptype, flags, int(round((lon - poi["lon0"]) * kx)), int(round((lat - poi["lat0"]) * ky)))


def poi_screen(data, lat, lon, mpp=DEFAULT_MPP, cx=RIDER_X, cy=RIDER_Y):
    """Every POI of a .poi as [(type, flags, x, y)] in panel px around the
    rider at (lat, lon), north up, computed as map_tiles projectPoisInto does
    (the file's own origin and scales, float maths)."""
    p = read_poi(data)
    px, py = (lon - p["lon0"]) * p["kx"], (lat - p["lat0"]) * p["ky"]
    out = []
    for t, f, x, y in p["records"]:
        out.append((t, f, cx + (x - px) / mpp, cy - (y - py) / mpp))
    return out


def poi_latlon(poi, rec):
    """A record of `poi` (a read_poi() dict) back to (lat, lon)."""
    return (poi["lat0"] + rec[3] / poi["ky"], poi["lon0"] + rec[2] / poi["kx"])
