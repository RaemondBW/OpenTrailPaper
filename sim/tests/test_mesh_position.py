"""Sharing the rider's own position on the mesh: off unless asked, per
channel, only with a real fix, and no more often than the community's
smart-position guidance allows (src/config.h MESH_POS_*: at most every 10
min and only after 100 m of movement, plus an hourly heartbeat when
parked). Read back as a stock node would read it (otpsim.mesh).

These run the device clock fast (`speed`) to cover hours of riding."""
import pytest

from mesh_kit import Mesh, is_port, keep_artifacts, mesh, start_device  # noqa: F401
from otpsim import pb
from otpsim import mesh as M

HOOD_RIVER = (45.70531, -121.52147)
MIN = 60 * 1000


def metres_north(lat, lon, m):
    return lat + m / 111320.0, lon


def positions(mesh_, **kw):
    return [h for h in mesh_.sent(**kw) if is_port(h, M.PORT_POSITION)]


def share(mesh_, index=0, on=True):
    mesh_.command(share=pb.MeshShare(index=index, on=on))


def test_the_riders_position_never_goes_out_unless_sharing_is_on(mesh):
    """The firmware's one statement about where the rider is, and on the
    public channel it goes to everyone in radio range: with a good fix,
    mesh on and hours passing, nothing may be sent until the rider turns
    sharing on for a channel."""
    mesh.dev.gps(*HOOD_RIVER, alt=150, sats=11)
    mesh.dev.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    assert not any(c.share_location for c in mesh.device_channels())
    mesh.dev.set_speed(200)
    mesh.dev_sleep(90 * MIN)
    mesh.dev.set_speed(1)
    assert positions(mesh, since=0) == []


def test_sharing_sends_the_fix_at_once_in_a_form_stock_nodes_plot(mesh):
    """Turning sharing on sends now (waiting out the interval reads as
    broken). The packet is a broadcast POSITION_APP on that channel, with
    the coordinates to 1e-7 degrees, altitude, the fix time, satellites, and
    location_source internal — what a stock node puts on its map."""
    lat, lon = HOOD_RIVER
    mesh.dev.gps(lat, lon, alt=152, sats=11)
    mesh.dev.wait_status(lambda s: s["ride"]["gps_fix"] and abs(s["ride"]["lat"] - lat) < 1e-6, timeout=10)
    share(mesh)
    assert {c.index: c.share_location for c in mesh.device_channels()}[0]
    p = mesh.wait_sent(lambda h: is_port(h, M.PORT_POSITION), "a position")
    assert p["dest"] == M.BROADCAST and p["channel"] == M.Channel().hash and not p["want_ack"]
    pos = M.decode_position(p["data"]["payload"])
    assert abs(pos["lat"] - lat) < 2e-7 and abs(pos["lon"] - lon) < 2e-7
    assert pos["alt"] == 152 and pos["sats"] == 11 and pos["location_source"] == 1
    assert pos["time"] > 1_700_000_000, "the phone set the clock; the fix should carry the time"


def test_no_fix_means_no_position_until_there_is_one(mesh):
    """Indoors or in a canyon: sharing is on, but a stale or invented
    position is worse than none to someone trying to find the rider. The
    first real fix goes out straight away."""
    mesh.dev.gps(*HOOD_RIVER, fix=False, sats=2)
    share(mesh)
    mesh.dev.set_speed(200)
    mesh.dev_sleep(15 * MIN)
    mesh.dev.set_speed(1)
    assert positions(mesh, since=0) == []
    mesh.dev.gps(*HOOD_RIVER, sats=8)
    p = mesh.wait_sent(lambda h: is_port(h, M.PORT_POSITION), "the first position after the fix")
    assert abs(M.decode_position(p["data"]["payload"])["lat"] - HOOD_RIVER[0]) < 2e-7


def test_position_updates_follow_the_ten_minute_and_100_m_rule_with_an_hourly_heartbeat(mesh):
    """A position is spent on everyone's airtime, so: parked, one an hour;
    moving, one when BOTH ten minutes have passed and the rider is 100 m
    from the last one sent — never more often however fast they ride, never
    for a 50 m shuffle in the car park."""
    lat, lon = HOOD_RIVER
    dev = mesh.dev
    dev.gps(lat, lon, sats=10)
    dev.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    share(mesh)
    first = mesh.wait_sent(lambda h: is_port(h, M.PORT_POSITION), "the first position")
    t0 = first["tx"]["start_ms"]
    dev.set_speed(200)
    # Moving 500 m within the first ten minutes: nothing yet.
    dev.gps(*metres_north(lat, lon, 500), sats=10)
    mesh.dev_sleep(8 * MIN)
    assert positions(mesh) == [], "a position inside the 10 minute minimum"
    # Past ten minutes, 500 m away: one goes out.
    mesh.dev_sleep(3 * MIN)
    moved = positions(mesh)
    assert len(moved) == 1
    # (Timed from when each was queued; on the air it can start a moment later.)
    assert moved[0]["tx"]["start_ms"] - t0 >= 10 * MIN - 2000
    assert abs(M.decode_position(moved[0]["data"]["payload"])["lat"] - (lat + 500 / 111320.0)) < 2e-6
    t1 = moved[0]["tx"]["start_ms"]
    # A 50 m shuffle, for 40 minutes: below the distance trigger.
    dev.gps(*metres_north(lat, lon, 550), sats=10)
    mesh.dev_sleep(40 * MIN)
    assert positions(mesh) == [], "a position for 50 m of movement"
    # Parked: the heartbeat, an hour after the last one sent.
    mesh.dev_sleep(25 * MIN)
    beat = positions(mesh)
    dev.set_speed(1)
    assert len(beat) == 1
    assert 60 * MIN - 2000 <= beat[0]["tx"]["start_ms"] - t1 <= 62 * MIN


def test_sharing_on_a_private_channel_stays_off_the_public_one(mesh):
    """Sharing with the ride group only: the position goes out on the
    group's channel, encrypted with its key, and the public channel —
    everyone else in range — hears nothing it can read. Forgetting the
    channel stops the sharing with it."""
    key = bytes(range(16, 32))
    mesh.set_private(1, "Group Ride", key)
    mesh.dev.gps(*HOOD_RIVER, sats=9)
    mesh.dev.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    share(mesh, index=1)
    p = mesh.wait_sent(lambda h: is_port(h, M.PORT_POSITION), "the group position")
    assert p["channel"] == M.Channel("Group Ride", key).hash
    public = M.VirtualNode(0x0EA20002, "Ear2", "EA2")
    assert public.decode(p["frame"])["data"] is None
    assert len(positions(mesh, since=0)) == 1, "also sent on the public channel"

    mesh.command(delete_channel=1)
    mesh.dev_wait(lambda: all(c.index != 1 for c in mesh.device_channels()), "channel deleted")
    mesh.set_private(1, "Group Ride", key)   # back, but sharing is not
    assert not {c.index: c.share_location for c in mesh.device_channels()}[1]
    mesh.dev.set_speed(200)
    mesh.dev_sleep(70 * MIN)
    mesh.dev.set_speed(1)
    assert positions(mesh) == []


@pytest.mark.xfail(strict=True, reason=(
    "BUG (privacy; may be a design choice): location sharing is a bitmask over channel SLOTS "
    "(mesh_service.cpp posMask), and neither applyChannelChange() nor applyPrivateChannel() (replacing a "
    "slot) clears the slot's bit. A rider sharing with a private primary channel who switches the primary "
    "back to the public default keeps broadcasting their position — now to everyone on the public mesh. On "
    "a stock node sharing/precision is part of the channel's own settings and goes with it"))
def test_replacing_the_channel_a_rider_shares_with_does_not_carry_the_sharing_over(mesh):
    """The rider agreed to tell one group where they are. When that channel
    is replaced by another (here the primary going back to the public
    default), the new audience has not been agreed to."""
    mesh.command(primary=pb.MeshPrimary(name="Club Ride", key_index=3))
    mesh.dev_wait(lambda: mesh.state().channel_name == "Club Ride", "private primary")
    mesh.dev.gps(*HOOD_RIVER, sats=9)
    mesh.dev.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    share(mesh, index=0)
    mesh.channels = [M.Channel("Club Ride", b"\x03")]
    mesh.wait_sent(lambda h: is_port(h, M.PORT_POSITION), "the club position", timeout=20)
    mesh.command(primary=pb.MeshPrimary(name="", key_index=1))       # back to public LongFast
    mesh.dev_wait(lambda: mesh.state().channel_name == "LongFast", "public primary")
    mesh.channels = [M.Channel()]
    mesh.dev.gps(*metres_north(*HOOD_RIVER, 400), sats=9)
    mesh.dev.set_speed(200)
    mesh.dev_sleep(70 * MIN)
    mesh.dev.set_speed(1)
    leaked = positions(mesh)
    assert leaked == [], "position broadcast on the public channel %d times" % len(leaked)
