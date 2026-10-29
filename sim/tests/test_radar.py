"""Rear radar (src/radar.cpp, the radar half of src/ble_sensors.cpp and the
RADAR tile in src/ui_render.cpp): a Varia-style radar's tracks reaching the
dashboard and the app, cars passing and expiring, a radar that goes quiet or
out of range, and the tile's alarm state on the glass.

The radar is the sim's virtual one (sim/PROTOCOL.md `sensor` kind `radar`):
Varia legacy packets at 4 Hz, split across continuation packets past six
tracks."""
import time

import pytest

from conftest import TIME_SCALE
from otpsim import pb
from sensorkit import add_and_pair, eventually, glass, holds, links, scan_until_listed, set_dash, settle, state

RIDE, RADAR = pb.LIVE_TOPIC_RIDE, pb.LIVE_TOPIC_RADAR
RADAR_DASH = "power3s large\nhr medium\ncadence medium\nradar\n"
RAIL = (356, 64, 508, 264)          # DASH_VERTICAL_X..+W, the top of the rail: its header


def targets(device):
    return [t["m"] for t in state(device)["radar"]["targets"]]


def radar_up(device, link, **values):
    addr = add_and_pair(device, link, "radar", name="RTL515", **values)
    eventually(lambda: state(device)["radar"], lambda r: r["connected"] and r["live"],
               "the radar never connected and went live", 25)
    return addr


def test_radar_tracks_reach_the_app_nearest_first(device, link):
    """The app's RADAR topic carries every track's distance, nearest first —
    whatever order the radar lists them in."""
    radar_up(device, link, targets=[80, 25, 140])
    link.expect_result(live_subscribe=pb.LiveSubscribe(topics=[RIDE, RADAR]))
    f = link.wait_push("live", lambda f: f.HasField("radar") and len(f.radar.distances_m) == 3,
                       timeout=10 * TIME_SCALE)
    assert list(f.radar.distances_m) == [25, 80, 140] and f.radar.live, f.radar
    assert f.ride.radar_connected and f.ride.radar_live


def test_eight_cars_split_across_two_packets_are_all_tracked(device, link):
    """A notification holds six tracks; a busy road sends the rest in a
    continuation packet. All eight must be on the tile, not the last six or
    the first six."""
    radar_up(device, link, targets=[15, 30, 45, 60, 75, 90, 105, 120])
    eventually(lambda: targets(device), lambda t: t == [15, 30, 45, 60, 75, 90, 105, 120],
               "the tracks from the continuation packet were lost", 10)
    holds(lambda: targets(device), lambda t: len(t) == 8, "tracks flickered between packets", 3)


def test_a_car_that_passes_leaves_the_tile_at_once(device, link):
    """Distance 0 is the radar saying 'it passed you': that track goes on the
    next packet, not after the two-second expiry."""
    addr = radar_up(device, link, targets=[[1, 20], [2, 90]])
    eventually(lambda: targets(device), lambda t: t == [20, 90], "the two cars never showed", 10)
    device.sensor("radar", addr=addr, targets=[[1, 0], [2, 88]])
    t0 = time.time()
    eventually(lambda: targets(device), lambda t: t == [88], "the car that passed stayed on the tile", 3)
    assert time.time() - t0 < 1.5 * TIME_SCALE, "the passed car lingered until the track expiry"


def test_a_car_no_longer_reported_expires(device, link):
    """A track the radar stops listing (turned off, fell back out of range)
    expires on its own after about two seconds; the others stay."""
    addr = radar_up(device, link, targets=[[5, 60], [6, 110]])
    eventually(lambda: targets(device), lambda t: t == [60, 110], "the cars never showed", 10)
    device.sensor("radar", addr=addr, targets=[[6, 105]])
    eventually(lambda: targets(device), lambda t: t == [105], "the unlisted car never expired", 5)


def test_a_radar_that_goes_quiet_reads_no_signal_and_is_reconnected(device, link):
    """Heartbeats stop (a radar that hangs, or a link the stack still thinks
    is up): within two seconds the tile stops showing tracks it can't vouch
    for, and after five the firmware drops and rebuilds the link. When the
    stream resumes, so do the tracks."""
    addr = radar_up(device, link, targets=[35])
    since = device.log_seq()
    device.sensor("radar", addr=addr, sending=False)
    r = eventually(lambda: state(device)["radar"], lambda r: not r["live"], "a silent radar stayed live", 4)
    assert r["targets"] == [], "a silent radar still shows its last car: %s" % r
    device.wait_log(r"radar stream silent; reconnecting", timeout=10 * TIME_SCALE, since=since)
    device.sensor("radar", addr=addr, sending=True, targets=[45])
    eventually(lambda: state(device)["radar"], lambda r: r["live"] and [t["m"] for t in r["targets"]] == [45],
               "the radar's stream never came back after the reconnect", 20)


def test_a_radar_out_of_range_reads_offline_and_reconnects(device, link):
    """The radar's link drops (battery out, unclipped): RADAR_CONNECTED goes
    false for the app and the tile; back in range it reconnects on its own."""
    addr = radar_up(device, link, targets=[70])
    device.remove_sensor(addr)
    eventually(lambda: state(device)["radar"], lambda r: not r["connected"] and r["targets"] == [],
               "the vanished radar still reads connected", 10)
    link.wait_push("live", lambda f: not f.ride.radar_connected, timeout=5 * TIME_SCALE)
    device.sensor("radar", addr=addr, name="RTL515", targets=[66])
    eventually(lambda: targets(device), lambda t: t == [66], "the radar never came back", 30)


def test_the_radar_tile_raises_its_alarm_for_a_close_car(device, link, tmp_path):
    """CAR BACK: a car within 40 m fills the tile's header in black, the one
    thing on the dashboard meant to be seen in peripheral vision. A car far
    back, or a clear road, leaves it white."""
    addr = radar_up(device, link, targets=[120])
    assert "radar" in set_dash(link, RADAR_DASH)
    device.wait_status(lambda s: s["screen"] == "dash")
    time.sleep(1.5)
    settle(device)
    far = glass(device, tmp_path, "far").ink(*RAIL)
    device.sensor("radar", addr=addr, targets=[[1, 120], [2, 25]])
    eventually(lambda: targets(device), lambda t: t == [25, 120], "the close car never showed", 10)
    time.sleep(1.5)
    settle(device)
    near = glass(device, tmp_path, "near").ink(*RAIL)
    assert near > far + 0.2, "a car 25 m back didn't raise the tile's alarm (ink %.2f vs %.2f)" % (near, far)
    device.sensor("radar", addr=addr, targets=[])
    eventually(lambda: targets(device), lambda t: t == [], "the road never cleared", 5)
    time.sleep(1.5)
    settle(device)
    clear = glass(device, tmp_path, "clear").ink(*RAIL)
    assert clear < near - 0.2, "the alarm stayed up on a clear road (ink %.2f)" % clear


def test_a_varia_advertising_only_garmins_uuid_is_offered_as_a_radar(device, link):
    """Some Varia adverts list only Garmin's member UUID (0xFE1F), not the
    radar service. A device named like a radar is offered as one; a Garmin
    watch advertising the same UUID is not a radar."""
    varia = device.sensor("radar", addr="c0:ff:ee:00:05:01", name="RTL64894", adv_uuids=["fe1f"])
    watch = device.sensor("radar", addr="c0:ff:ee:00:05:02", name="Forerunner 965", adv_uuids=["fe1f"])
    l = scan_until_listed(link, varia)
    row = [s for s in l.sensors if s.address == varia][0]
    assert row.kinds == 1 << 3, row
    time.sleep(2)
    l = link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_LIST)).sensor_list
    assert not any(s.address == watch for s in l.sensors), "a Garmin watch was offered as a radar"
