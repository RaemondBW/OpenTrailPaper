"""Maps on the device, with real tiles: the Hood River fixture
(sim/fixtures/tiles/, otpsim.tiles) sent over the link the way the apps send
it, listed back, stored, replaced, deleted and drawn.

What is drawn is checked on the glass: the amount of ink against the
"NO MAP HERE" page, and the fixture's own roads, projected the way
src/map_tiles.cpp projects them, must land on inked pixels (and must not when
the projection is moved 30 m). The firmware's `timing` console switch logs
every map frame (polys per class, tiles drawn/wanted, " cut" for a partial
frame); the tests read those too."""
import math
import os
import re
import struct
import time
import zlib

import pytest

from conftest import paired_phone
from otpsim import LinkClient, LinkError, SimDevice, pb
from otpsim.tiles import (MAJOR, MINOR, PATH, PRIMARY, SECONDARY, TERTIARY, card_path, fixture_manifest,
                          fixture_pois, fixture_tiles, keep_roads, samples_along, screen_roads)
from test_regressions_ui import Glass

MANIFEST = fixture_manifest()
CENTRE = (MANIFEST["centre"]["lat"], MANIFEST["centre"]["lon"])
CENTRE_H3 = MANIFEST["centre"]["h3"]
TILES = fixture_tiles()
POIS = fixture_pois()
BLOBS = [d for _, d, _ in TILES]
STREETS = (MAJOR, PRIMARY, SECONDARY, TERTIARY, MINOR)

# The "NO MAP HERE" box (map_view.cpp ui_render_map: 500x96, centred, 30 px up).
NOMAP_TOP = (960 - 96) // 2 - 30


# -- the app's side -------------------------------------------------------------------------

def send_tiles(link, tiles, target=pb.TARGET_MAP_TILE, timeout=30.0):
    """BLEManager.swift sendNextTile over the link: one upload per file, in
    order, named by its H3 id, with its version record when the device keeps
    them (FEATURE_MAP_TILE_VERSIONS); a BUSY device gets the tile back on
    the queue a little later."""
    versions = link.has(pb.FEATURE_MAP_TILE_VERSIONS)
    for h3, data, version in tiles:
        extra = {"tile_version": version} if versions and version else {}
        for attempt in range(12):
            try:
                link.upload(target, h3, data, timeout=timeout, **extra)
                break
            except LinkError as e:
                if e.code != "RESULT_BUSY":
                    raise
                time.sleep(0.5)
        else:
            raise AssertionError("tile %s: device stayed busy" % h3)


def send_region(link, tiles=TILES, pois=POIS):
    """A map download from the Maps screen: the tiles, then their .poi files
    (to firmware that lists FEATURE_MAP_POI), then the lists the app asks
    for to learn what the device holds."""
    send_tiles(link, tiles)
    if link.has(pb.FEATURE_MAP_POI):
        send_tiles(link, pois, pb.TARGET_MAP_POI)


def listed(link, kind=pb.LIST_MAP_TILES):
    """{h3: version record} from the device's tile (or POI) list, all pages."""
    pages = link.request(list=pb.ListRequest(kind=kind), all_pages=True, timeout=10)
    out = {}
    for p in pages:
        assert p.WhichOneof("body") == "tile_list", p
        for t in p.tile_list.tiles:
            out[t.h3] = t.version
    return out


# -- the map page ---------------------------------------------------------------------------

FRAME = re.compile(r"map: project=\d+ draw=\d+ polys=(\d+) \(tiles=(\d+) base=\d+\) "
                   r"cls\[maj=(\d+) pri=(\d+) sec=(\d+) ter=(\d+) min=(\d+) path=(\d+)\] "
                   r"ntiles=(\d+)/(\d+)( cut)? mpp=(\d+)")


def parse_frame(line):
    m = FRAME.search(line)
    if not m:
        return None
    g = [int(x) if x and x.strip().isdigit() else x for x in m.groups()]
    return dict(polys=g[0], tile_polys=g[1], cls=dict(zip(("maj", "pri", "sec", "ter", "min", "path"), g[2:8])),
                drawn=g[8], wanted=g[9], cut=bool(m.group(11)), mpp=g[11])


def frames(device, since):
    return [f for f in (parse_frame(l) for l in device.log(since)) if f]


def wait_frame(device, since, pred=lambda f: True, timeout=10):
    """The first complete map frame (not cut short) after `since` that
    matches `pred`."""
    deadline = time.time() + timeout
    while True:
        for f in frames(device, since):
            if not f["cut"] and pred(f):
                return f
        if time.time() > deadline:
            raise AssertionError("no complete map frame matching; last: %s" % (frames(device, since)[-3:],))
        time.sleep(0.2)


def open_map(device, lat, lon):
    device.gps(lat, lon)
    device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    device.console("timing")              # log every map frame
    seq = device.log_seq()
    device.console("screen map")
    device.wait_status(lambda s: s["screen"] == "map", timeout=5)
    return seq


def glass(device, tmp_path, name):
    path = str(tmp_path / (name + ".png"))
    device.screenshot(path)
    return Glass(path)


def map_ink(g):
    """Ink over the map viewport (status bar and footer strip left out)."""
    return g.ink(0, 70, 540, 805)


def shows_no_map(g):
    """The 'NO MAP HERE' box: its 2 px frame across the panel (left of the
    zoom buttons), white inside above the text."""
    return (g.ink(30, NOMAP_TOP - 1, 450, NOMAP_TOP + 1) > 0.95 and
            g.ink(30, NOMAP_TOP + 95, 450, NOMAP_TOP + 97) > 0.95 and
            g.ink(30, NOMAP_TOP + 4, 450, NOMAP_TOP + 20) < 0.02)


def safe(x, y):
    """Map pixels nothing else is drawn over: not the rider marker, the
    compass, the zoom/layers buttons or the scale bar."""
    return (10 <= x <= 450 and 150 <= y <= 790 and math.hypot(x - 270, y - 430) > 45
            and not (x < 140 and y > 740))


def road_hits(g, lat, lon, blobs=BLOBS, classes=STREETS, mpp=2):
    """Of the fixture's roads around (lat, lon), sampled every 6 px along
    each, the fraction with ink within 2 px. (fraction, samples)."""
    n = hit = 0
    for _, xy in screen_roads(blobs, lat, lon, mpp=mpp, classes=classes):
        for x, y in samples_along(xy):
            if not safe(x, y):
                continue
            n += 1
            xi, yi = int(round(x)), int(round(y))
            if any(g.px(xi + dx, yi + dy) <= 0x40 for dx in range(-2, 3) for dy in range(-2, 3)):
                hit += 1
    return hit / max(n, 1), n


def moved(lat, lon, north_m, east_m):
    return (lat + north_m / 110540.0, lon + east_m / (111320.0 * math.cos(math.radians(lat))))


def over_usb(device, edit):
    """A computer mounts the card, `edit(sd_root)` changes it, the computer
    lets go. Held until the device has noticed (its UI loop polls the host
    once per frame) and has rescanned its maps."""
    seq = device.log_seq()
    device.usb_host(True)
    device.wait_log(r"map: project=", since=seq, timeout=10)
    edit(device.sd)
    seq = device.log_seq()
    device.usb_host(False)
    device.wait_log(r"map: \d+ whole maps indexed", since=seq, timeout=10)


def alive(device):
    assert device.proc.poll() is None, "the sim exited (%s): see %s" % (device.proc.returncode, device.log_path)
    device.status()


# -- tests ----------------------------------------------------------------------------------

def test_the_region_uploads_as_the_app_sends_it_and_lists_back(device, link):
    """The whole fixture as a map download sends it: every tile and .poi
    saved byte for byte where the firmware reads them, and listed back with
    the version record each was sent with."""
    assert link.has(pb.FEATURE_MAP_TILES) and link.has(pb.FEATURE_MAP_TILE_VERSIONS)
    assert listed(link) == {}
    send_region(link)
    assert listed(link) == {h3: v for h3, _, v in TILES}
    assert listed(link, pb.LIST_MAP_POIS) == {h3: v for h3, _, v in POIS}
    for h3, data, _ in TILES:
        with open(card_path(device.sd, h3), "rb") as f:
            assert f.read() == data, h3
    for h3, data, _ in POIS:
        with open(card_path(device.sd, h3, ".poi"), "rb") as f:
            assert f.read() == data, h3
    # A new session (the app relaunched, its own records gone) sees the same.
    phone = link.phone
    phone.disconnect()
    phone.connect()
    assert phone.wait_security(10)["encrypted"]
    assert listed(LinkClient(phone).hello()) == {h3: v for h3, _, v in TILES}


def test_the_map_draws_the_region_at_its_centre(device, link, tmp_path):
    """No map, then the region arrives and the map page draws it: far more
    ink than the empty page, and the town's streets where they are."""
    lat, lon = CENTRE
    seq = open_map(device, lat, lon)
    wait_frame(device, seq, lambda f: f["drawn"] == 0)
    empty = glass(device, tmp_path, "empty")
    assert shows_no_map(empty)

    seq = device.log_seq()
    send_region(link)
    f = wait_frame(device, seq, lambda f: f["polys"] > 0)
    assert f["drawn"] == f["wanted"] >= 1
    assert min(f["cls"].values()) > 0, "every road class is in downtown Hood River: %s" % f["cls"]
    time.sleep(0.5)
    g = glass(device, tmp_path, "map")
    assert not shows_no_map(g)
    assert map_ink(g) > 4 * map_ink(empty) and map_ink(g) > 0.06, (map_ink(g), map_ink(empty))

    hit, n = road_hits(g, lat, lon)
    assert n > 500, "the fixture should put plenty of streets on this page: %d samples" % n
    assert hit > 0.95, "streets missing where the tiles put them: %.0f%% inked" % (hit * 100)
    assert road_hits(empty, lat, lon)[0] < 0.2
    off, _ = road_hits(g, *moved(lat, lon, 30, 30))
    assert off < 0.5, "the check is too lax: streets 40 m off still 'hit' %.0f%%" % (off * 100)

    # Zoomed out the page needs the neighbours too, and gets every one.
    seq = device.log_seq()
    for _ in range(3):
        device.console("zoom out")
    f = wait_frame(device, seq, lambda f: f["mpp"] == 16)
    assert f["drawn"] == f["wanted"] >= 3 and f["polys"] > 100, f
    g = glass(device, tmp_path, "wide")
    assert not shows_no_map(g) and map_ink(g) > 0.06
    hit, n = road_hits(g, lat, lon, mpp=16, classes=(MAJOR, PRIMARY, SECONDARY, TERTIARY))
    assert n > 300 and hit > 0.9, "zoomed out, %.0f%% of %d samples inked" % (hit * 100, n)
    alive(device)


def test_outside_the_region_the_map_says_no_map_here(device, link, tmp_path):
    send_region(link)
    out = MANIFEST["outside"]
    seq = open_map(device, out["lat"], out["lon"])
    wait_frame(device, seq)
    g = glass(device, tmp_path, "outside")
    assert shows_no_map(g)
    assert map_ink(g) < 0.04
    # Back in town, the map again.
    seq = device.log_seq()
    device.gps(*CENTRE)
    wait_frame(device, seq, lambda f: f["polys"] > 100)
    time.sleep(0.5)
    assert not shows_no_map(glass(device, tmp_path, "back"))


def test_riding_across_tile_boundaries_keeps_drawing(device, link, tmp_path):
    """The manifest's track, 10 km west to east through the centre and
    across three cells, at 2 m/px. Ridden at 60 km/h (the clock 20x): every
    frame draws roads and completes with all the tiles it wanted. Then
    stopped every 500 m along it: the page shows the fixture's roads and
    trails where they are, on both sides of every tile boundary."""
    send_region(link)
    tr = MANIFEST["track"]
    assert len(tr["cells"]) >= 3
    (lat0, lon0), (lat1, lon1) = tr["from"], tr["to"]
    along = lambda t: (lat0 + (lat1 - lat0) * t, lon0 + (lon1 - lon0) * t)
    seq = open_map(device, lat0, lon0)
    wait_frame(device, seq, lambda f: f["polys"] > 0)

    seq = device.log_seq()
    device.set_speed(20)
    device.gps_track([along(i / 100.0) for i in range(101)], speed_kmh=60)
    st = device.wait_status(lambda s: s["ride"]["lon"] >= lon1 - 0.0005, timeout=90, interval=0.5)
    device.set_speed(1)
    alive(device)
    fs = frames(device, seq)
    assert len(fs) >= 100, "only %d map frames over 10 minutes of riding" % len(fs)
    empty = [f for f in fs if f["polys"] == 0]
    assert not empty, "%d of %d frames drew nothing: %s" % (len(empty), len(fs), empty[:3])
    short = [f for f in fs if not f["cut"] and f["drawn"] != f["wanted"]]
    assert not short, short[:3]

    worst = []
    for i in range(21):
        lat, lon = along(i / 20.0)
        device.gps(lat, lon)
        device.wait_status(lambda s: abs(s["ride"]["lon"] - lon) < 1e-5, timeout=10)
        seq = device.log_seq()
        wait_frame(device, seq)
        time.sleep(0.3)
        g = glass(device, tmp_path, "step%02d" % i)
        assert not shows_no_map(g), "no map at %.5f,%.5f" % (lat, lon)
        hit, n = road_hits(g, lat, lon, classes=STREETS + (PATH,))
        assert n >= 30, "the fixture has roads everywhere on this track: %d samples at step %d" % (n, i)
        worst.append((hit, i, n))
    assert min(worst)[0] > 0.9, "roads missing at (hit, step, samples): %s" % sorted(worst)[:3]


def test_a_newer_version_of_a_tile_replaces_it(device, link, tmp_path):
    """The CDN's next build of the centre cell drops its minor streets and
    paths: sent with its new record, the device lists the new record and
    draws the new tile; the old one sent back restores both."""
    lat, lon = CENTRE
    send_region(link)
    seq = open_map(device, lat, lon)
    before = wait_frame(device, seq, lambda f: f["polys"] > 0)
    old = dict((h, (d, v)) for h, d, v in TILES)[CENTRE_H3]
    newer = keep_roads(old[0], {MAJOR, PRIMARY, SECONDARY, TERTIARY})
    assert len(newer) < len(old[0])
    rec = "cdn:%016x" % zlib.crc32(newer)

    seq = device.log_seq()
    send_tiles(link, [(CENTRE_H3, newer, rec)])
    assert listed(link)[CENTRE_H3] == rec
    f = wait_frame(device, seq, lambda f: f["cls"]["min"] == 0)
    assert f["cls"]["path"] == 0 and f["cls"]["maj"] == before["cls"]["maj"], (before, f)
    time.sleep(0.5)
    g = glass(device, tmp_path, "newer")
    assert road_hits(g, lat, lon, classes=(MAJOR, PRIMARY))[0] > 0.95
    gone, n = road_hits(g, lat, lon, blobs=[old[0]], classes=(MINOR,))
    assert n > 200 and gone < 0.5, "the old tile's side streets are still drawn: %.0f%%" % (gone * 100)
    with open(card_path(device.sd, CENTRE_H3), "rb") as fh:
        assert fh.read() == newer

    seq = device.log_seq()
    send_tiles(link, [(CENTRE_H3, old[0], old[1])])
    assert listed(link)[CENTRE_H3] == old[1]
    wait_frame(device, seq, lambda f: f["cls"]["min"] == before["cls"]["min"])
    time.sleep(0.5)
    assert road_hits(glass(device, tmp_path, "restored"), lat, lon, classes=(MINOR,))[0] > 0.95


def test_deleting_tiles(device, link, tmp_path):
    """The link deletes rides, routes and workouts, not tiles (the apps never
    ask): a tile delete is UNSUPPORTED and changes nothing. A rider deletes
    tiles from the card over USB; when the computer lets go of the card the
    device rescans it, stops listing them and shows no map there."""
    lat, lon = CENTRE
    send_region(link)
    r = link.request(delete_file=pb.DeleteRequest(kind=pb.LIST_MAP_TILES, name=CENTRE_H3))
    assert r.WhichOneof("body") == "result" and r.result.code == pb.RESULT_UNSUPPORTED, r
    assert CENTRE_H3 in listed(link)

    seq = open_map(device, lat, lon)
    wait_frame(device, seq, lambda f: f["polys"] > 0)

    def delete_centre(sd):
        os.remove(card_path(sd, CENTRE_H3))
        os.remove(card_path(sd, CENTRE_H3, ".poi"))
    seq = device.log_seq()
    over_usb(device, delete_centre)
    wait_frame(device, seq, lambda f: f["drawn"] == 0)
    time.sleep(0.5)
    assert shows_no_map(glass(device, tmp_path, "deleted"))
    assert listed(link) == {h3: v for h3, _, v in TILES if h3 != CENTRE_H3}
    assert CENTRE_H3 not in listed(link, pb.LIST_MAP_POIS)

    # Everything gone: an empty list; and a tile sent again later without a
    # record is not credited with the deleted file's.
    def delete_all(sd):
        for h3, _, _ in TILES:
            p = card_path(sd, h3)
            if os.path.exists(p):
                os.remove(p)
    over_usb(device, delete_all)
    assert listed(link) == {}
    send_tiles(link, [(CENTRE_H3, dict((h, d) for h, d, _ in TILES)[CENTRE_H3], "")])
    assert listed(link) == {CENTRE_H3: ""}


# -- big transfers ----------------------------------------------------------------------------

def newer_record(rec):
    """Another valid CDN record for the same cell ("cdn:" + 16 hex)."""
    return "cdn:" + rec[4:][::-1]


def timed_send(link, tiles):
    t = time.time()
    send_tiles(link, tiles, timeout=120)
    return time.time() - t


def report(what, n, size, secs, raw=None):
    rate = size / 1024.0 / max(secs, 1e-6)
    line = "%s: %d tiles, %d KB in %.1f s (%.1f KB/s%s)" % (
        what, n, size // 1024, secs, rate, ", link carries %.1f KB/s" % (raw / 1024.0) if raw else "")
    print("\n" + line)
    return line


def test_the_whole_region_five_times_over_a_normal_link(device, link):
    """80 tiles, ~1.9 MB: the fixture five times over, the last time with
    new records, as a phone at a 15 ms interval sends them (6 packets per
    event; ~96 KB/s on air). All saved, listed with the newest records, and
    the transfer goes at the link's pace, not the device's."""
    chunk = link.phone.att_mtu - 3
    raw = 6 * chunk / 0.015
    link.phone.pace_writes(15, 6)
    n = size = 0
    t = time.time()
    for rnd in range(5):
        tiles = [(h, d, v if rnd < 4 else newer_record(v)) for h, d, v in TILES]   # last round: new records
        send_tiles(link, tiles, timeout=120)
        n += len(tiles)
        size += sum(len(d) for _, d, _ in tiles)
    secs = time.time() - t
    report("normal link", n, size, secs, raw)
    assert listed(link) == {h: newer_record(v) for h, _, v in TILES}
    assert size / secs > 0.5 * raw, "the device held the link up: %.1f KB/s of %.1f" % (
        size / secs / 1024, raw / 1024)
    alive(device)


def test_tiles_over_a_slow_link():
    """A 400 ms connection interval with 4 packets per event both ways (the
    link of test_download_survives_a_slow_link), ~2.4 KB/s: the three
    smallest tiles still go through without the device giving up, at no
    worse than half the link's pace."""
    with SimDevice.start(args=["--gps-baud", "115200", "--conn-interval-ms", "400",
                               "--packets-per-event", "4", "--notify-window", "4"]) as dev:
        phone = paired_phone(dev)
        link = LinkClient(phone).hello(timeout=15)
        chunk = phone.att_mtu - 3
        raw = 4 * chunk / 0.4
        phone.pace_writes(400, 4)
        tiles = sorted(TILES, key=lambda t: len(t[1]))[:3]
        size = sum(len(d) for _, d, _ in tiles)
        secs = timed_send(link, tiles)
        report("slow link", len(tiles), size, secs, raw)
        assert listed(link) == {h: v for h, _, v in tiles}
        assert size / secs > 0.5 * raw, "%.2f KB/s of %.2f" % (size / secs / 1024, raw / 1024)
        assert not [l for l in dev.log() if "notify failed" in l]


# -- bad tiles ---------------------------------------------------------------------------------

def test_a_tile_that_is_not_a_tile_is_refused(device, link):
    """Wrong magic, or a .poi sent as a tile: refused with PARSE_ERROR,
    nothing stored, nothing listed."""
    for bad in (b"EBM1" + TILES[0][1][4:], b"junk" * 40, POIS[0][1]):
        with pytest.raises(LinkError) as e:
            link.upload(pb.TARGET_MAP_TILE, CENTRE_H3, bad, tile_version="cdn:bad")
        assert e.value.code == "RESULT_PARSE_ERROR"
    assert listed(link) == {}
    assert not os.path.exists(card_path(device.sd, CENTRE_H3))


@pytest.mark.parametrize("damage", ["truncated", "noise"])
def test_a_damaged_tile_does_not_take_the_map_down(device, link, tmp_path, damage):
    """A tile whose header is fine but whose body is cut short or noise (the
    link's CRC only proves it arrived as sent). The firmware checks only the
    header, so it is stored; the map page must draw what it can of it and the
    neighbours, keep running, and recover when the good tile is sent."""
    lat, lon = CENTRE
    good = dict((h, d) for h, d, _ in TILES)[CENTRE_H3]
    bad = good[:len(good) // 2] if damage == "truncated" else good[:36] + os.urandom(len(good) - 36)
    send_tiles(link, [t for t in TILES if t[0] != CENTRE_H3] + [(CENTRE_H3, bad, "cdn:damaged")])
    seq = open_map(device, lat, lon)
    wait_frame(device, seq)
    for _ in range(3):                         # zoom out and back: every tile path
        device.console("zoom out")
    for _ in range(3):
        device.console("zoom in")
    time.sleep(2)
    alive(device)
    g = glass(device, tmp_path, "damaged")
    assert not shows_no_map(g)
    seq = device.log_seq()
    send_tiles(link, [(CENTRE_H3, good, "cdn:good")])
    wait_frame(device, seq, lambda f: f["polys"] > 200 and f["mpp"] == 2)
    time.sleep(0.5)
    assert road_hits(glass(device, tmp_path, "fixed"), lat, lon)[0] > 0.95


@pytest.mark.xfail(strict=True, reason=(
    "firmware bug: saveTile accepts any EBM2 header, and map_tiles projectBlobInto trusts its "
    "grid size (nx*ny index entries) without checking it against the blob length, so a tile whose "
    "header claims a larger grid than the file holds is read far out of bounds: the sim segfaults "
    "on the first map frame (on the board: a panic, again every time the map opens there)"))
def test_a_tile_whose_grid_overruns_the_file_does_not_crash_the_map(device, link):
    lat, lon = CENTRE
    good = bytearray(dict((h, d) for h, d, _ in TILES)[CENTRE_H3])
    struct.pack_into("<ii", good, 28, 30000, 30000)
    send_tiles(link, [(CENTRE_H3, bytes(good), "cdn:bad-grid")])
    seq = open_map(device, lat, lon)
    time.sleep(3)
    alive(device)
    wait_frame(device, seq)
