"""Riding with a private channel's group, on the device itself: where the
others are (their shared positions as rings on the map with the first two
letters of their name, pinned to the edge and pointing their way when off
the screen), and their messages popping up over the ride screens for a few
seconds. Only private channels: the public one is strangers in
radio range.

The map frame log (`timing`) counts the dots drawn (`peers=N`); the panel
itself is read back from a screenshot."""
import re
import time

from conftest import TIME_SCALE
from mesh_kit import keep_artifacts, mesh, start_device  # noqa: F401
from otpsim import mesh as M
from test_regressions_ui import Glass

HOOD_RIVER = (45.70531, -121.52147)
KEY = bytes(range(1, 33))
ALEX = 0xA1E50001
BEA = 0xBEA00002
PEERS = re.compile(r"map: .* at=(-?\d+\.\d+),(-?\d+\.\d+) peers=(\d+)")
# The map at its default 2 m/px, north up, the rider marker at (270, 430).
RIDER = (270, 430)
MPP = 2.0
BAND = (0, 70, 540, 200)            # the top band: turn, pause, message


def alex(**kw):
    return M.VirtualNode(ALEX, "Alex Rivera", "ALX", **kw)


def bea(**kw):
    return M.VirtualNode(BEA, "Bea", "BEA", **kw)


def north(m):
    return HOOD_RIVER[0] + m / 111320.0, HOOD_RIVER[1]


def join_group(mesh):
    mesh.set_private(1, "Sunday Ride", KEY)
    return M.Channel("Sunday Ride", KEY)


def introduce(mesh, node):
    """Its NodeInfo, so the device has a name for it."""
    mesh.hear(node.nodeinfo())
    mesh.drain()


def ride(mesh, screen):
    mesh.dev.gps(*HOOD_RIVER, sats=9)
    mesh.dev.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    mesh.dev.console("timing")
    mesh.dev.console("screen " + screen)
    mesh.dev.wait_status(lambda s: s["screen"] == screen, timeout=5)


def wait_peers(dev, since, n, timeout=10):
    """The first map frame after `since` showing `n` dots, once it is painted
    (the next frame line or a refresh after it)."""
    deadline = time.time() + timeout * TIME_SCALE
    while True:
        lines = dev.log(since)
        for i, l in enumerate(lines):
            m = PEERS.search(l)
            if m and int(m.group(3)) == n and any("refresh:" in x or PEERS.search(x) for x in lines[i + 1:]):
                return
        if time.time() > deadline:
            seen = [PEERS.search(l).group(3) for l in lines if PEERS.search(l)]
            raise AssertionError("no painted map frame with %d peers; frames had %s" % (n, seen[-5:]))
        time.sleep(0.1)


def glass(mesh, tmp_path, name):
    path = str(tmp_path / (name + ".png"))
    mesh.dev.screenshot(path)
    return Glass(path)


def wait_band(mesh, tmp_path, up, timeout=5):
    """Screenshots until the top band is (up) or is not (not up) on the
    glass. Polled: the repaint can land before or after any marker the
    test could take."""
    deadline = time.time() + timeout * TIME_SCALE
    while True:
        ink = glass(mesh, tmp_path, "band").ink(*BAND)
        if (ink > 0.6) == up:
            return
        if time.time() > deadline:
            raise AssertionError("band %s; ink %.2f" % ("never appeared" if up else "still up", ink))
        time.sleep(0.2)


def dot_ink(g, x, y, r=6):
    return g.ink(x - r, y - r, x + r, y + r)


def ring_ink(g, x, y):
    """The left and right of a rider's ring (26 px out, 4 px wide)."""
    return min(g.ink(x - 25, y - 3, x - 23, y + 3), g.ink(x + 23, y - 3, x + 25, y + 3))


# ---- positions on the map ----------------------------------------------------------------

def test_a_group_riders_shared_position_is_a_dot_on_the_device_map(mesh, tmp_path):
    group = join_group(mesh)
    ride(mesh, "map")
    seq = mesh.dev.log_seq()
    assert ring_ink(glass(mesh, tmp_path, "before"), RIDER[0], RIDER[1] - 150) < 0.5

    introduce(mesh, alex(channel=group))
    mesh.hear(alex(channel=group).position(*north(300)))
    mesh.drain()
    wait_peers(mesh.dev, seq, 1)
    g = glass(mesh, tmp_path, "with-alex")
    assert ring_ink(g, RIDER[0], RIDER[1] - 150) > 0.8, "no ring 300 m north of the rider"
    # Hollow, with "Al" in it.
    assert 0.1 < dot_ink(g, RIDER[0], RIDER[1] - 150, r=8) < 0.8


def test_positions_from_the_public_channel_stay_off_the_map(mesh, tmp_path):
    join_group(mesh)
    ride(mesh, "map")
    seq = mesh.dev.log_seq()
    introduce(mesh, bea())
    mesh.hear(bea().position(*north(300)))
    mesh.drain()
    assert any(n.num == BEA and n.HasField("position") for n in mesh.nodes()), "the position did arrive"
    mesh.dev.console("zoom in")        # one more frame, after the position
    mesh.dev.console("zoom out")
    wait_peers(mesh.dev, seq, 0)
    assert all(int(m.group(3)) == 0 for m in (PEERS.search(l) for l in mesh.dev.log(seq)) if m)
    assert ring_ink(glass(mesh, tmp_path, "public"), RIDER[0], RIDER[1] - 150) < 0.5


def test_a_rider_off_the_screen_is_pinned_to_the_edge_pointing_their_way(mesh, tmp_path):
    group = join_group(mesh)
    ride(mesh, "map")
    seq = mesh.dev.log_seq()
    mesh.hear(alex(channel=group).position(*north(20000)))     # 20 km up the road
    mesh.drain()
    wait_peers(mesh.dev, seq, 1)
    g = glass(mesh, tmp_path, "far")
    # Pinned straight up from the rider, under the status bar, its pointer
    # out of the top of the ring.
    marker = (RIDER[0], 64 + 84)
    assert ring_ink(g, *marker) > 0.8, "no marker at the top edge"
    assert dot_ink(g, marker[0], marker[1] - 42, r=4) > 0.8, "no pointer above it"
    assert dot_ink(g, marker[0], marker[1] + 42, r=4) < 0.5, "pointer the wrong way"
    # How far, on a chip beside it.
    assert g.ink(marker[0] + 36, marker[1] - 10, marker[0] + 90, marker[1] + 10) > 0.05
    assert ring_ink(g, RIDER[0], RIDER[1] - 150) < 0.5


def test_forgetting_the_channel_takes_its_riders_off_the_map(mesh, tmp_path):
    group = join_group(mesh)
    ride(mesh, "map")
    seq = mesh.dev.log_seq()
    mesh.hear(alex(channel=group).position(*north(300)))
    mesh.drain()
    wait_peers(mesh.dev, seq, 1)

    seq = mesh.dev.log_seq()
    mesh.command(delete_channel=1)
    mesh.dev_wait(lambda: all(c.index != 1 for c in mesh.device_channels()), "channel deleted")
    mesh.dev.console("zoom in")
    mesh.dev.console("zoom out")
    wait_peers(mesh.dev, seq, 0)
    assert ring_ink(glass(mesh, tmp_path, "forgotten"), RIDER[0], RIDER[1] - 150) < 0.5


# ---- message popups ----------------------------------------------------------------------

def test_a_group_message_pops_up_over_the_ride_screen_then_goes(mesh, tmp_path):
    group = join_group(mesh)
    ride(mesh, "dash")
    assert glass(mesh, tmp_path, "before").ink(*BAND) < 0.5
    introduce(mesh, alex(channel=group))
    seq = mesh.dev.log_seq()
    mesh.hear(alex(channel=group).text("Regroup at the cafe"))
    mesh.dev.wait_log(r"ui: mesh notice from Alex Rivera on Sunday Ride", since=seq)
    wait_band(mesh, tmp_path, True)
    mesh.dev_sleep(7000)                # MESH_NOTICE_MS is 10 s
    assert glass(mesh, tmp_path, "still").ink(*BAND) > 0.6, "the band left early"
    mesh.dev_sleep(4000)
    wait_band(mesh, tmp_path, False)


def test_tapping_the_message_dismisses_it(mesh, tmp_path):
    group = join_group(mesh)
    ride(mesh, "map")
    introduce(mesh, alex(channel=group))
    seq = mesh.dev.log_seq()
    mesh.hear(alex(channel=group).text("Flat tyre, go on"))
    mesh.dev.wait_log(r"ui: mesh notice from Alex Rivera", since=seq)
    wait_band(mesh, tmp_path, True)
    mesh.dev.tap(270, 130)
    wait_band(mesh, tmp_path, False, timeout=3)    # well inside its 10 s


def test_public_channel_messages_do_not_pop_up(mesh, tmp_path):
    join_group(mesh)
    ride(mesh, "dash")
    seq = mesh.dev.log_seq()
    mesh.hear(bea().text("anyone out today?"))
    mesh.drain()
    mesh.dev_wait(lambda: any(x.text == "anyone out today?" for x in mesh.history()), "the message arrived")
    mesh.dev_sleep(1500)
    assert not any("mesh notice" in l for l in mesh.dev.log(seq))
    assert glass(mesh, tmp_path, "public").ink(*BAND) < 0.5
