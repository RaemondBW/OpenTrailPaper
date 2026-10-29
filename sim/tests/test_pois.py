"""Cycling POIs on the device: the .poi file next to each map tile (format in
src/map_tiles.cpp, writer docs/mapgen.js buildPoi), sent over the link as the
apps send it (TARGET_MAP_POI, the tile's H3 id), stored under
/maps/tiles/<prefix>/<rest>.poi, read with its tile into the tile cache
(map_store ensureTileLoaded), projected (map_tiles projectPoisInto) and drawn
as icons over the map and the route (map_view drawPois).

The POIs are the synthetic sets of sim/fixtures/tiles/pois/ (build_pois.mjs:
made-up OSM nodes encoded by the real buildPoi, over the real Hood River
tiles), plus the CDN's own .poi files for those cells. What is drawn is
checked on the glass: each icon is compared with the one the MAP LAYERS
screen draws for its type, at the place the file puts it. The `timing`
console switch logs every map frame with `pois=<kept>/<dropped>`.

The firmware has no other use for POIs: the ride recorder and navigation
never read them (forEachPoi has no caller in src/), so "POIs in a ride" is
the map page drawing them while a ride records and a route is followed."""
import math
import os
import re
import time

import pytest

from conftest import TIME_SCALE
from otpsim import LinkError, pb
from otpsim.tiles import (BIKE_SHOP, PF_PUMP, PF_RESTRICTED, REPAIR, TOILETS, WATER, card_path, fixture_manifest,
                          fixture_pois, poi_record, poi_screen, poi_set, read_poi, write_poi)
from test_maps import TILES, alive, glass, listed, moved, open_map, over_usb, parse_frame, send_tiles

MANIFEST = fixture_manifest()
NAMES = {WATER: "water", TOILETS: "toilets", REPAIR: "repair", BIKE_SHOP: "bike shop"}
LAYER = {WATER: 1 << 2, TOILETS: 1 << 3, REPAIR: 1 << 4, BIKE_SHOP: 1 << 5}
ALL_LAYERS = 0x3F

# map_view.cpp drawPois: icon radius, the rider marker's clear zone, chrome.
POI_R = 12
RIDER = (270, 430)
MAP_TOP, MAP_BOTTOM = 64, 810

# Where the CDN's POIs are thickest on one 2 m/px page: downtown Hood River,
# ten of them.
DOWNTOWN = (45.7119, -121.5131)


# -- the app's side ---------------------------------------------------------------------------

def send_pois(link, pois):
    send_tiles(link, pois, pb.TARGET_MAP_POI)


def refused(link, h3, data, target=pb.TARGET_MAP_POI):
    with pytest.raises(LinkError) as e:
        link.upload(target, h3, data, tile_version="cdn:%016x" % 1)
    return e.value.code


# -- map frames -------------------------------------------------------------------------------

POIS = re.compile(r" pois=(\d+)/(\d+)")


def poi_frames(device, since):
    out = []
    for line in device.log(since):
        f = parse_frame(line)
        m = POIS.search(line)
        if f and m:
            f["pois"], f["dropped"] = int(m.group(1)), int(m.group(2))
            out.append(f)
    return out


def wait_pois(device, since, pred=lambda f: True, timeout=10, fresh=False):
    """The first complete map frame after `since` matching `pred`; `fresh`:
    not the first frame logged after `since`, which may have been projected
    before it (a frame is logged after it is drawn)."""
    deadline = time.time() + timeout * TIME_SCALE
    while True:
        for f in poi_frames(device, since)[1 if fresh else 0:]:
            if not f["cut"] and pred(f):
                return f
        if time.time() > deadline:
            raise AssertionError("no complete map frame matching; last: %s" % (poi_frames(device, since)[-3:],))
        time.sleep(0.2)


def settled(device, tmp_path, name, since, pred=lambda f: True):
    """Wait for a matching frame, give the panel the paint, read the glass."""
    f = wait_pois(device, since, pred, fresh=True)
    time.sleep(0.6)
    return f, glass(device, tmp_path, name)


def rider(device):
    st = device.status()["ride"]
    return st["lat"], st["lon"]


def zoom(device, steps):
    """Zoom out (steps > 0) or in, a step (x2) at a time; the log position
    before it."""
    seq = device.log_seq()
    for _ in range(abs(steps)):
        device.console("zoom out" if steps > 0 else "zoom in")
    return seq


# -- icons on the glass -----------------------------------------------------------------------

R = 10      # compared inside this radius: icon fill and glyph only (the ring and halo lie outside)
DISC = [(dx, dy) for dy in range(-R, R + 1) for dx in range(-R, R + 1) if dx * dx + dy * dy <= R * R]
# The pump's plus on a repair stand (drawPoiIcon): left out of the stand's comparison.
PLUS = {(dx, dy) for dx in range(-8, -1) for dy in (-7, -6)} | {(dx, dy) for dx in (-5, -4) for dy in range(-10, -3)}

_REFS = {}


def references(device, tmp_path):
    """The icon of each type as the MAP LAYERS screen draws its sample (rows
    2-5 at x 70, map_view.cpp drawLayerSample): what the map must show.
    Solid (no flags). The firmware draws them, so they are read once."""
    if not _REFS:
        before = device.status()["paints"]
        device.console("screen layers")
        device.wait_status(lambda s: s["paints"] > before, timeout=5 * TIME_SCALE)
        time.sleep(0.5)
        g = glass(device, tmp_path, "layers")
        for row, t in ((2, WATER), (3, TOILETS), (4, REPAIR), (5, BIKE_SHOP)):
            cx, cy = 70, 64 + row * 108 + 54
            _REFS[t] = {(dx, dy): g.px(cx + dx, cy + dy) <= 0x40 for dx, dy in DISC}
        # Sanity: four different glyphs.
        for a in _REFS:
            for b in _REFS:
                if a < b:
                    assert similarity(_REFS[a], _REFS[b]) < 0.95, (NAMES[a], NAMES[b])
    return _REFS


def similarity(a, b, skip=()):
    keys = [k for k in DISC if k not in skip]
    return sum(a[k] == b[k] for k in keys) / len(keys)


def icon_at(g, x, y, ref, hollow=False, skip=(), slack=2):
    """Best agreement (0..1) of the glass around (x, y), within `slack` px,
    with a reference icon; a hollow icon (MAP_PF_RESTRICTED) is its inverse."""
    best = 0.0
    xi, yi = int(round(x)), int(round(y))
    for oy in range(-slack, slack + 1):
        for ox in range(-slack, slack + 1):
            got = {k: (g.px(xi + ox + k[0], yi + oy + k[1]) <= 0x40) != hollow for k in DISC}
            best = max(best, similarity(got, ref, skip))
    return best


def plus_ink(g, x, y, hollow=False):
    """How much of the pump's plus is in the stand's glyph colour (white on
    a solid icon), best over ±1 px."""
    best = 0.0
    for oy in (-1, 0, 1):
        for ox in (-1, 0, 1):
            n = sum((g.px(int(round(x)) + ox + dx, int(round(y)) + oy + dy) > 0x40) != hollow for dx, dy in PLUS)
            best = max(best, n / len(PLUS))
    return best


def assert_icon(g, refs, poi, x, y, what=""):
    """The icon for `poi` (type, flags) is at (x, y): it matches its own
    type's icon, and better than any other type's."""
    t, flags = poi[0], poi[1]
    hollow = bool(flags & PF_RESTRICTED)
    skip = PLUS if t == REPAIR else ()
    scores = {u: icon_at(g, x, y, refs[u], hollow, skip) for u in refs}
    best = max(scores, key=scores.get)
    assert best == t and scores[t] > 0.95, "%s %s at (%.0f, %.0f): %s" % (
        what, NAMES[t], x, y, {NAMES[u]: round(s, 2) for u, s in scores.items()})
    if t == REPAIR:
        p = plus_ink(g, x, y, hollow)
        if flags & PF_PUMP:
            assert p > 0.9, "%s: the pump's plus is missing (%.2f)" % (what, p)
        else:
            assert p < 0.2, "%s: a plus on a stand without a pump (%.2f)" % (what, p)


def no_icon(g, refs, x, y, what=""):
    scores = {NAMES[u]: round(max(icon_at(g, x, y, refs[u]), icon_at(g, x, y, refs[u], True)), 2) for u in refs}
    assert max(scores.values()) < 0.85, "%s: an icon at (%.0f, %.0f): %s" % (what, x, y, scores)


def expected_icons(pois, mpp=2, layers=ALL_LAYERS, nav=False):
    """Which of `pois` [(type, flags, x, y)] (projected, in the frame's order)
    map_view drawPois draws: priority order repair > water > toilets > shop,
    each kept off the chrome, the rider marker and the icons already drawn."""
    gap = 4 * POI_R if mpp >= 8 else 2 * POI_R + 4
    pad = POI_R + 3
    comp_y = 64 + 138 + 34 + 14 if nav else 64 + 48

    def under_chrome(x, y):
        if (x - (540 - 46)) ** 2 + (y - comp_y) ** 2 < (28 + pad) ** 2:
            return True
        if x > 540 - 78 - pad and y > 560 - 76 - 8 - pad and y < 640 + 76 + pad:
            return True
        if x < 190 + pad and y > MAP_BOTTOM - 48 - pad:
            return True
        return nav and y < 64 + 138 + pad

    drawn = []
    for t in (REPAIR, WATER, TOILETS, BIKE_SHOP):
        for p in pois:
            x, y = int(math.floor(p[2] + 0.5)), int(math.floor(p[3] + 0.5))
            if p[0] != t or not layers & LAYER[t]:
                continue
            if y < MAP_TOP + POI_R or y > MAP_BOTTOM - POI_R or x < POI_R or x > 540 - POI_R:
                continue
            if (x - RIDER[0]) ** 2 + (y - RIDER[1]) ** 2 < 36 * 36 or under_chrome(x, y):
                continue
            if any(abs(dx - x) < gap and abs(dy - y) < gap for _, _, dx, dy in drawn):
                continue
            drawn.append((p[0], p[1], x, y))
    return drawn


def check_icons(g, refs, files, lat, lon, mpp=2, layers=ALL_LAYERS, nav=False, what=""):
    """Every icon the frame should draw for `files` (.poi bytes, nearest tile
    first) is on the glass with the right glyph. Returns them."""
    pois = []
    for data in files:
        pois += [p for p in poi_screen(data, lat, lon, mpp) if p[0] in NAMES and visible_at(p[0], mpp)]
    want = expected_icons(pois, mpp, layers, nav)
    for t, f, x, y in want:
        assert_icon(g, refs, (t, f), x, y, what)
    return want


def visible_at(t, mpp):
    """map_tiles poiAtZoom."""
    return mpp <= 4 or (mpp <= 8 and t in (WATER, REPAIR))


# -- tests ------------------------------------------------------------------------------------

def test_pois_upload_and_draw_with_the_right_icon_where_they_are(device, link, tmp_path):
    """The `types` set (one POI of each type and flag that changes the icon)
    sent as the app sends it: listed back with its record, stored byte for
    byte, and on the map page each icon is the right glyph — solid, hollow
    for a fee, a plus for a stand with a pump — at the place the file puts
    it; 30 m off, none is."""
    refs = references(device, tmp_path)
    meta, files = poi_set("types")
    assert link.has(pb.FEATURE_MAP_POI)
    send_tiles(link, TILES)
    send_pois(link, files)
    assert listed(link, pb.LIST_MAP_POIS) == {h3: v for h3, _, v in files}
    for h3, data, _ in files:
        with open(card_path(device.sd, h3, ".poi"), "rb") as f:
            assert f.read() == data

    lat, lon = meta["stand"]["lat"], meta["stand"]["lon"]
    seq = open_map(device, lat, lon)
    f, g = settled(device, tmp_path, "types", seq, lambda f: f["polys"] > 0)
    kept = [p for p in meta["pois"] if p["type"]]
    assert f["pois"] == len(kept) == 7 and f["dropped"] == 0, f
    lat, lon = rider(device)
    want = check_icons(g, refs, [d for _, d, _ in files], lat, lon, what="types")
    assert sorted((t, fl) for t, fl, _, _ in want) == sorted((p["type"], p["flags"]) for p in kept), \
        "every POI of the set should be drawn at 2 m/px: %s" % want
    # The check is not lax: with the rider 30 m off, nothing matches.
    for t, fl, x, y in want:
        assert icon_at(g, x + 15, y - 15, refs[t], bool(fl & PF_RESTRICTED)) < 0.85
    alive(device)


def test_the_cdn_pois_are_drawn_where_they_are(device, link, tmp_path):
    """The fixture's real .poi files (the CDN's, from OSM), with the region:
    downtown's toilets, taps and shops each drawn as its own icon at its
    place, the frame holding every one on the page."""
    refs = references(device, tmp_path)
    send_tiles(link, TILES)
    send_pois(link, fixture_pois())
    lat, lon = DOWNTOWN
    seq = open_map(device, lat, lon)
    f, g = settled(device, tmp_path, "cdn", seq, lambda f: f["polys"] > 0)
    lat, lon = rider(device)
    files = [d for h, d, _ in fixture_pois()]
    on_page = [p for d in files for p in poi_screen(d, lat, lon)
               if -16 <= p[2] <= 556 and -16 <= p[3] <= 976]
    assert f["pois"] == len(on_page) >= 10 and f["dropped"] == 0, (f, len(on_page))
    # Icons whose place no other icon competes for (the order icons of one
    # type are drawn in depends on the order the tiles were read).
    isolated = [p for p in on_page
                if not any(q is not p and abs(q[2] - p[2]) < 40 and abs(q[3] - p[3]) < 40 for q in on_page)]
    want = [w for w in expected_icons(isolated) if any(abs(w[2] - p[2]) < 1 and abs(w[3] - p[3]) < 1 for p in isolated)]
    assert len(want) >= 3, want
    for t, fl, x, y in want:
        assert_icon(g, refs, (t, fl), x, y, "cdn")


def test_zoom_and_layers_choose_which_pois_are_drawn(device, link, tmp_path):
    """poiAtZoom: every type to 4 m/px, water and repair stands at 8, none
    wider. The MAP LAYERS switches (set from the phone) hide a type and bring
    it back."""
    refs = references(device, tmp_path)
    meta, files = poi_set("types")
    send_tiles(link, TILES)
    send_pois(link, files)
    lat, lon = meta["stand"]["lat"], meta["stand"]["lon"]
    seq = open_map(device, lat, lon)
    wait_pois(device, seq, lambda f: f["pois"] == 7)
    lat, lon = rider(device)
    data = [d for _, d, _ in files]

    seq = zoom(device, 1)
    f, g = settled(device, tmp_path, "z4", seq, lambda f: f["mpp"] == 4)
    assert f["pois"] == 7, f
    assert len(check_icons(g, refs, data, lat, lon, mpp=4, what="4 m/px")) >= 5
    seq = zoom(device, 1)
    f = wait_pois(device, seq, lambda f: f["mpp"] == 8)
    n8 = sum(1 for p in meta["pois"] if p["type"] in (WATER, REPAIR))
    assert f["pois"] == n8 == 4, f
    seq = zoom(device, 1)
    f = wait_pois(device, seq, lambda f: f["mpp"] == 16)
    assert f["pois"] == 0, f
    seq = zoom(device, -3)
    wait_pois(device, seq, lambda f: f["mpp"] == 2 and f["pois"] == 7)

    # The rider hides drinking water; the rest stays.
    r = link.request(set_map_layers=pb.MapLayers(mask=ALL_LAYERS & ~LAYER[WATER]))
    assert r.map_layers.mask == ALL_LAYERS & ~LAYER[WATER], r
    seq = device.log_seq()
    _, g = settled(device, tmp_path, "nowater", seq)
    shown = check_icons(g, refs, data, lat, lon, layers=ALL_LAYERS & ~LAYER[WATER], what="no water")
    assert shown and WATER not in [t for t, _, _, _ in shown]
    for t, fl, x, y in expected_icons([p for d in data for p in poi_screen(d, lat, lon)]):
        if t == WATER:
            no_icon(g, refs, x, y, "hidden water")
    r = link.request(set_map_layers=pb.MapLayers(mask=ALL_LAYERS))
    seq = device.log_seq()
    _, g = settled(device, tmp_path, "water", seq)
    assert WATER in [t for t, _, _, _ in check_icons(g, refs, data, lat, lon, what="layers back")]


def test_pois_either_side_of_a_tile_edge(device, link, tmp_path):
    """A tap in one cell and a stand in the next, 120 m apart across the
    edge: both drawn, each from its own tile's file. A builder that keeps a
    cell's whole bounding box puts the tap in the neighbour's file too: still
    one icon, not two (projectPoisInto drops the duplicate)."""
    refs = references(device, tmp_path)
    meta, files = poi_set("edge")
    assert len(files) == 2
    send_tiles(link, TILES)
    send_pois(link, files)
    lat, lon = meta["stand"]["lat"], meta["stand"]["lon"]
    seq = open_map(device, lat, lon)
    f, g = settled(device, tmp_path, "edge", seq, lambda f: f["polys"] > 0 and f["pois"] > 0)
    assert f["pois"] == 2 and f["drawn"] == f["wanted"], f
    lat, lon = rider(device)
    want = check_icons(g, refs, [d for _, d, _ in files], lat, lon, what="edge")
    assert sorted(t for t, _, _, _ in want) == [WATER, REPAIR], want

    # The tap again, in the neighbour's file.
    tap = next(p for p in meta["pois"] if p["type"] == WATER)
    other = next((h, d) for h, d, _ in files if h != tap["cell"])
    p = read_poi(other[1])
    both = write_poi(p["lat0"], p["lon0"], p["records"] + [poi_record(p, WATER, 0, tap["lat"], tap["lon"])],
                     cell=p["cell"], kx=p["kx"], ky=p["ky"])
    seq = device.log_seq()
    send_pois(link, [(other[0], both, "phone:2")])
    f, g = settled(device, tmp_path, "dup", seq, lambda f: f["pois"] > 0)
    assert f["pois"] == 2, "the tap twice: %s" % f
    check_icons(g, refs, [d for _, d, _ in files], lat, lon, what="dup")


def test_replacing_and_deleting_a_tiles_pois(device, link, tmp_path):
    """A newer .poi for the centre cell replaces the old (the tap moved 100 m
    east, the shop gone): the device lists the new record and draws the new
    file, nothing where the old icons were. An empty .poi ("built, none
    here") draws none. The link deletes no POIs (UNSUPPORTED, as for tiles);
    a computer deleting the file over USB does, and the list drops it."""
    refs = references(device, tmp_path)
    meta, files = poi_set("types")
    send_tiles(link, TILES)
    send_pois(link, files)
    (h3, data, _), = files
    lat, lon = meta["stand"]["lat"], meta["stand"]["lon"]
    seq = open_map(device, lat, lon)
    wait_pois(device, seq, lambda f: f["pois"] == 7)
    lat, lon = rider(device)
    old = expected_icons(poi_screen(data, lat, lon))

    p = read_poi(data)
    recs = []
    for r in p["records"]:
        if r[0] == BIKE_SHOP:
            continue
        if r[0] == WATER and not r[1]:
            r = (r[0], r[1], r[2] + 100, r[3])
        recs.append(r)
    newer = write_poi(p["lat0"], p["lon0"], recs, cell=p["cell"], kx=p["kx"], ky=p["ky"])
    seq = device.log_seq()
    send_pois(link, [(h3, newer, "phone:7")])
    assert listed(link, pb.LIST_MAP_POIS)[h3] == "phone:7"
    with open(card_path(device.sd, h3, ".poi"), "rb") as f:
        assert f.read() == newer
    f, g = settled(device, tmp_path, "newer", seq, lambda f: f["pois"] == 6)
    now = check_icons(g, refs, [newer], lat, lon, what="newer")
    assert BIKE_SHOP not in [t for t, _, _, _ in now]
    for t, fl, x, y in old:
        if t == BIKE_SHOP or (t == WATER and not fl):
            no_icon(g, refs, x, y, "old " + NAMES[t])

    empty = write_poi(p["lat0"], p["lon0"], [], cell=p["cell"], kx=p["kx"], ky=p["ky"])
    seq = device.log_seq()
    send_pois(link, [(h3, empty, "phone:8")])
    f, g = settled(device, tmp_path, "empty", seq, lambda f: f["pois"] == 0)
    for t, fl, x, y in now:
        no_icon(g, refs, x, y, "emptied " + NAMES[t])
    assert listed(link, pb.LIST_MAP_POIS) == {h3: "phone:8"}

    send_pois(link, files)
    r = link.request(delete_file=pb.DeleteRequest(kind=pb.LIST_MAP_POIS, name=h3))
    assert r.WhichOneof("body") == "result" and r.result.code == pb.RESULT_UNSUPPORTED, r
    assert h3 in listed(link, pb.LIST_MAP_POIS)
    seq = device.log_seq()
    over_usb(device, lambda sd: os.remove(card_path(sd, h3, ".poi")))
    f, g = settled(device, tmp_path, "deleted", seq, lambda f: f["polys"] > 0)
    assert f["pois"] == 0, f
    assert listed(link, pb.LIST_MAP_POIS) == {}
    assert h3 in listed(link)                 # the tile stays
    for t, fl, x, y in old:
        no_icon(g, refs, x, y, "deleted " + NAMES[t])
    alive(device)


def test_tiles_without_poi_files_and_pois_without_their_tile(device, link, tmp_path):
    """Tiles built before POIs (or by a builder with none) have no .poi: the
    map draws them with no icons. A .poi whose tile is not on the card
    draws nothing either (it is read with its tile); when the tile arrives,
    its POIs come with it."""
    refs = references(device, tmp_path)
    meta, files = poi_set("types")
    (h3, data, ver), = files
    send_tiles(link, [t for t in TILES if t[0] != h3])
    assert listed(link, pb.LIST_MAP_POIS) == {}
    lat, lon = meta["stand"]["lat"], meta["stand"]["lon"]
    send_pois(link, files)                    # the POIs first, their tile not yet
    seq = open_map(device, lat, lon)
    f = wait_pois(device, seq, fresh=True)
    assert f["pois"] == 0 and f["drawn"] == 0, f
    seq = zoom(device, 2)                     # 8 m/px: the neighbours, no POIs
    f = wait_pois(device, seq, lambda f: f["mpp"] == 8 and f["polys"] > 0)
    assert f["pois"] == 0, f
    zoom(device, -2)

    seq = device.log_seq()
    send_tiles(link, [t for t in TILES if t[0] == h3])
    f, g = settled(device, tmp_path, "arrived", seq, lambda f: f["pois"] == 7)
    lat, lon = rider(device)
    check_icons(g, refs, [data], lat, lon, what="tile arrived")

    # And every other tile with no .poi: the map at the track's west end.
    lat0, lon0 = MANIFEST["track"]["from"]
    device.gps(lat0, lon0)
    device.wait_status(lambda s: abs(s["ride"]["lon"] - lon0) < 1e-5, timeout=10 * TIME_SCALE)
    seq = device.log_seq()
    f = wait_pois(device, seq, lambda f: f["polys"] > 0, fresh=True)
    assert f["pois"] == 0 and f["drawn"] == f["wanted"], f
    alive(device)


def test_more_pois_than_a_frame_holds(device, link, tmp_path):
    """The `crowd` set: 300 toilets on a 20 m grid south of the rider, and a
    tap and a repair stand north of it, in one file — 302 in view, over the
    frame's 256. The frame says what it dropped, the device keeps running at
    every zoom, and what is kept is what matters most: the tap and the stand
    are drawn. (They were not: the first 256 in the file were kept, and a
    .poi is sorted south to north, so everything north of the rider went.)"""
    refs = references(device, tmp_path)
    meta, files = poi_set("crowd")
    send_tiles(link, TILES)
    send_pois(link, files)
    lat, lon = meta["stand"]["lat"], meta["stand"]["lon"]
    seq = open_map(device, lat, lon)
    f, g = settled(device, tmp_path, "crowd", seq, lambda f: f["polys"] > 0)
    assert f["pois"] == 256 and f["dropped"] == 302 - 256, f
    zoom(device, 4)
    zoom(device, -4)
    seq = device.log_seq()
    f, g = settled(device, tmp_path, "crowd2", seq, lambda f: f["mpp"] == 2)
    alive(device)
    lat, lon = rider(device)
    (_, data, _), = files
    for t, fl, x, y in poi_screen(data, lat, lon):
        if t in (WATER, REPAIR):
            assert_icon(g, refs, (t, fl), x, y, "north of a crowd")


# -- damaged files ----------------------------------------------------------------------------

def damaged(kind):
    """The centre cell's `types` file, damaged one way."""
    _, files = poi_set("types")
    data = files[0][1]
    p = read_poi(data)
    hdr = lambda **k: write_poi(**dict(dict(lat0=p["lat0"], lon0=p["lon0"], records=p["records"], cell=p["cell"],
                                            kx=p["kx"], ky=p["ky"]), **k))
    return {
        "not_epoi": lambda: b"EPOX" + data[4:],
        "version_2": lambda: hdr(version=2),
        "short_records": lambda: hdr(rec_size=4),
        "count_overruns": lambda: hdr(count=len(p["records"]) + 1000),
        "count_max": lambda: hdr(count=0xFFFF),
        "truncated": lambda: data[:len(data) - 3],
        "header_only_cut": lambda: data[:30],
        "nan_origin": lambda: hdr(lat0=float("nan")),
        "inf_scale": lambda: hdr(kx=float("inf")),
        "zero_scale": lambda: hdr(ky=0.0),
    }[kind]()


BAD = ["not_epoi", "version_2", "short_records", "count_overruns", "count_max", "truncated", "header_only_cut",
       "nan_origin", "inf_scale", "zero_scale"]


@pytest.mark.parametrize("kind", BAD)
def test_a_damaged_poi_file_is_refused_and_skipped_on_the_card(device, link, tmp_path, kind):
    """A .poi with a bad header, a count the file does not hold, or an origin
    or scale that places nothing: the link refuses it (PARSE_ERROR; nothing
    stored, nothing listed). The same file put on the card over USB (a
    computer, or firmware that took it) is skipped: no POIs from it, the tile
    under it drawn, the device running."""
    bad = damaged(kind)
    meta, files = poi_set("types")
    h3 = files[0][0]
    send_tiles(link, TILES)
    assert refused(link, h3, bad) == "RESULT_PARSE_ERROR"
    assert listed(link, pb.LIST_MAP_POIS) == {}
    assert not os.path.exists(card_path(device.sd, h3, ".poi"))

    def put(sd):
        with open(card_path(sd, h3, ".poi"), "wb") as f:
            f.write(bad)
    lat, lon = meta["stand"]["lat"], meta["stand"]["lon"]
    open_map(device, lat, lon)
    seq = device.log_seq()
    over_usb(device, put)
    f = wait_pois(device, seq, lambda f: f["polys"] > 0, fresh=True)
    assert f["pois"] == 0 and f["drawn"] == f["wanted"], f
    zoom(device, 2)
    seq = zoom(device, -2)
    wait_pois(device, seq, lambda f: f["mpp"] == 2)
    alive(device)


def test_unknown_types_and_longer_records_are_read_as_far_as_known(device, link, tmp_path):
    """Forward compatibility: types this firmware has no icon for (0, 5,
    200) in a file are skipped, the known ones drawn; and a file of a later
    layout whose records carry extra bytes (recordSize 8) is read with its own
    stride — every icon where it belongs."""
    refs = references(device, tmp_path)
    meta, files = poi_set("types")
    (h3, data, _), = files
    p = read_poi(data)
    send_tiles(link, TILES)
    lat, lon = meta["stand"]["lat"], meta["stand"]["lon"]
    seq = open_map(device, lat, lon)
    wait_pois(device, seq, lambda f: f["polys"] > 0)
    lat, lon = rider(device)

    strange = [poi_record(p, t, fl, *moved(lat, lon, n, e))
               for t, fl, n, e in ((0, 0, 100, -100), (5, 0, 100, 100), (200, 0xFF, -150, 0))]
    mixed = write_poi(p["lat0"], p["lon0"], strange + p["records"], cell=p["cell"], kx=p["kx"], ky=p["ky"])
    seq = device.log_seq()
    send_pois(link, [(h3, mixed, "phone:1")])
    f, g = settled(device, tmp_path, "unknown", seq, lambda f: f["pois"] > 0)
    assert f["pois"] == 7, f
    check_icons(g, refs, [data], lat, lon, what="beside unknown types")
    for t, fl, x, y in poi_screen(mixed, lat, lon)[:3]:
        no_icon(g, refs, x, y, "unknown type %d" % t)

    longer = write_poi(p["lat0"], p["lon0"], p["records"], cell=p["cell"], kx=p["kx"], ky=p["ky"], rec_size=8)
    seq = device.log_seq()
    send_pois(link, [(h3, longer, "phone:2")])
    f, g = settled(device, tmp_path, "rec8", seq, lambda f: f["pois"] > 0)
    assert f["pois"] == 7, f
    check_icons(g, refs, [data], lat, lon, what="recordSize 8")


# -- riding -----------------------------------------------------------------------------------

def test_pois_along_a_ride_on_a_route(device, link, tmp_path):
    """The `ride` set: a tap every km along the manifest's track (100 m
    north of it, across three cells) and one on the track itself. A route
    along the track is followed and a ride recorded while the rider goes
    east: each tap is drawn where it is as the rider passes it, the one on
    the route over the route's 14 px line (POIs are drawn after the route).
    POIs play no other part in a ride (nothing in the recorder or the
    navigation reads them): the ride records on, the device stays up."""
    from test_regressions_ui import accept_route, send_route

    refs = references(device, tmp_path)
    meta, files = poi_set("ride")
    send_tiles(link, TILES)
    send_pois(link, files)
    (la0, lo0), (la1, lo1) = MANIFEST["track"]["from"], MANIFEST["track"]["to"]
    along = lambda t: (la0 + (la1 - la0) * t, lo0 + (lo1 - lo0) * t)
    pts = [along(i / 200.0) for i in range(201)]
    open_map(device, *pts[0])
    send_route(link, "Hood", pts, [(la1, lo1, "Arrive")])
    accept_route(device)
    device.button("boot")                     # start recording
    device.wait_status(lambda s: s["ride"]["recording"], timeout=5 * TIME_SCALE)
    device.console("screen map")              # starting navigation shows the dashboard
    device.wait_status(lambda s: s["screen"] == "map", timeout=5 * TIME_SCALE)
    data = [d for _, d, _ in files]

    taps = sorted((p for p in meta["pois"] if p["type"] == WATER), key=lambda p: p["lon"])
    assert len(taps) == 10
    checked = 0
    for tap in taps:
        # 200 m short of the tap on the track, facing east. Stopped there (a
        # moving fix would carry the rider on), so the ride auto-pauses: it
        # still records.
        t = (tap["lon"] - lo0) / (lo1 - lo0) - 200 / 10000.0
        lat, lon = along(t)
        device.gps(lat, lon, speed_kmh=0, course=90)
        device.wait_status(lambda s: abs(s["ride"]["lon"] - lon) < 1e-5, timeout=10 * TIME_SCALE)
        seq = device.log_seq()
        f, g = settled(device, tmp_path, "ride_%s" % tap["name"], seq, lambda f: f["polys"] > 0)
        assert f["pois"] >= 1, f
        rl, ro = rider(device)
        shown = check_icons(g, refs, data, rl, ro, nav=True, what=tap["name"])
        here = [s for s in shown if abs(s[2] - (RIDER[0] + 100)) < 8]
        assert here, "%s: not drawn 100 px ahead of the rider: %s" % (tap["name"], shown)
        checked += 1
    assert checked == 10
    st = device.status()
    assert st["ride"]["recording"] and st["nav"]["active"], st
    alive(device)
