"""Street names on the device map: the tile's NAM1 section (layout in
docs/mapgen.js, writer buildEbm `names: true`), read with the roads
(map_tiles projectBlobInto) and drawn as upright labels at the closest zoom
only (map_view drawStreetNames, MAP_NAMES_MAX_MPP = 1 m/px). The `timing`
console switch logs every map frame with `names=<labels drawn>/<names in
view>`.

The tiles are the real Hood River ones (sim/fixtures/tiles) with names added
here by class (otpsim.tiles.with_names), so the count of distinct names in
view is known."""
import re

from otpsim.tiles import with_names
from test_maps import CENTRE, CENTRE_H3, POIS, TILES, alive, open_map, parse_frame, send_region

NAMES = re.compile(r" names=(\d+)/(\d+)")
BY_CLASS = {0: "Interstate 84", 1: "Oak St", 2: "State St", 3: "May St", 4: "Elm Ave"}


def by_class(sub, i, cls):
    return BY_CLASS.get(cls)


def region(centre):
    """The fixture region with the centre tile swapped for `centre`."""
    return [(h, centre if h == CENTRE_H3 else d, v + "-n" if h == CENTRE_H3 else v) for h, d, v in TILES]


def names_at(device, since, mpp, timeout=20):
    """(labels drawn, names in view) of the first complete frame at `mpp`
    logged after `since`."""
    import time
    deadline = time.time() + timeout
    while True:
        for l in device.log(since):
            f, m = parse_frame(l), NAMES.search(l)
            if f and m and not f["cut"] and f["mpp"] == mpp:
                return int(m.group(1)), int(m.group(2))
        if time.time() > deadline:
            raise AssertionError("no complete frame at %d m/px" % mpp)
        time.sleep(0.2)


def centre_tile():
    return next(d for h, d, _ in TILES if h == CENTRE_H3)


def test_street_names_are_drawn_at_the_closest_zoom_only(device, link):
    send_region(link, region(with_names(centre_tile(), by_class)), POIS)
    seq = open_map(device, *CENTRE)
    assert names_at(device, seq, 2) == (0, 0), "names read or drawn at 2 m/px"

    seq = device.log_seq()
    device.console("zoom in")
    labels, names = names_at(device, seq, 1)
    # Downtown at 1 m/px has primary, tertiary and minor roads in view.
    assert 3 <= names <= len(BY_CLASS), names
    assert 1 <= labels <= names, (labels, names)

    seq = device.log_seq()
    device.console("zoom out")
    assert names_at(device, seq, 2) == (0, 0)


def test_a_tile_without_names_draws_none(device, link):
    send_region(link, TILES, POIS)
    seq = open_map(device, *CENTRE)
    device.console("zoom in")
    assert names_at(device, seq, 1) == (0, 0)


def test_a_damaged_name_section_is_ignored_not_trusted(device, link):
    named = with_names(centre_tile(), by_class)
    at = named.rindex(b"NAM1") + 8
    # More names claimed than the section holds: every length read would run
    # off its end.
    damaged = named[:at] + b"\xff\xff" + named[at + 2:]
    send_region(link, region(damaged), POIS)
    seq = open_map(device, *CENTRE)
    device.console("zoom in")
    assert names_at(device, seq, 1) == (0, 0)
    alive(device)
