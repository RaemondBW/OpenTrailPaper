"""The phone link (docs/ble-link.md): session, settings, layout, live state,
paging and sensors — each checked on the device's side too."""
import re
import time

import pytest

from conftest import paired_phone
from otpsim import LinkClient, pb


def test_hello_and_the_initial_pushes(link):
    h = link.device_hello
    assert h.firmware.startswith("v")
    assert h.max_message == 4096
    assert link.has(pb.FEATURE_LIVE) and link.has(pb.FEATURE_SHOW_PAGE) and link.has(pb.FEATURE_DASH)
    for kind in ("settings", "dash", "live"):
        link.wait_push(kind, timeout=5)


def test_hello_sets_the_device_clock(device, phone):
    assert device.status()["wall_unix"] < 1_000_000   # the board boots with no clock
    LinkClient(phone).hello(unix_ms=1_767_225_600_000)   # 2026-01-01
    st = device.wait_status(lambda s: s["wall_unix"] >= 1_767_225_600)
    assert st["wall_unix"] < 1_767_225_600 + 60


def test_settings_round_trip_and_persist(device, link):
    r = link.request(set_settings=pb.Settings(ftp_w=280, use_miles=True, wheel_mm=2096))
    assert r.settings.ftp_w == 280 and r.settings.use_miles and r.settings.wheel_mm == 2096
    st = device.status()["settings"]
    assert st["ftp"] == 280 and st["miles"] and st["wheel_mm"] == 2096
    device.reboot()
    st = device.status()["settings"]
    assert st["ftp"] == 280 and st["miles"] and st["wheel_mm"] == 2096


def test_settings_out_of_range_are_not_applied(device, link):
    link.request(set_settings=pb.Settings(ftp_w=9000))
    assert device.status()["settings"]["ftp"] != 9000


def test_a_device_side_edit_is_pushed(device, link):
    link.drain_pushes()
    device.console("autopause 30")   # not a link setting: no push expected
    device.console("wheel 2150")
    s = link.wait_push("settings", lambda s: s.wheel_mm == 2150, timeout=5)
    assert s.wheel_mm == 2150


def test_dash_layout_from_the_app_reaches_the_panel(device, link, tmp_path):
    text = "page\nspeed hero\nhr half\ncadence half\n"
    r = link.request(set_dash=pb.DashConfig(text=text))
    assert r.WhichOneof("body") == "dash", r
    assert "speed" in r.dash.text
    st = device.wait_status(lambda s: re.search(r"^speed\s+hero", s["dash"], re.M))
    assert st["page_count"] >= 1
    device.screenshot(str(tmp_path / "layout.png"))


def test_a_bad_layout_is_refused(link):
    r = link.request(set_dash=pb.DashConfig(text="nonsense nonsense\n"))
    assert r.WhichOneof("body") == "result" and r.result.code == pb.RESULT_PARSE_ERROR


def test_show_page_pages_the_device(device, link):
    """The Ride tab's pager drives the device (link ShowPage, 2026-10-25)."""
    pages = device.status()["page_count"]
    assert pages >= 2
    link.tell(show_page=pb.ShowPage(page=1))
    st = device.wait_status(lambda s: s["page"] == 1 or s["screen"] == "map")
    link.tell(show_page=pb.ShowPage(page=0))
    st = device.wait_status(lambda s: s["page"] == 0 and s["screen"] == "dash")
    link.tell(show_page=pb.ShowPage(page=99))   # out of range: ignored
    time.sleep(0.5)
    assert device.status()["page"] == 0


def test_the_view_topic_follows_the_device(device, link):
    link.expect_result(live_subscribe=pb.LiveSubscribe(topics=[pb.LIVE_TOPIC_RIDE, pb.LIVE_TOPIC_VIEW]))
    link.tell(show_page=pb.ShowPage(page=1))
    frame = link.wait_push("live", lambda f: f.HasField("view") and f.view.HasField("page") and f.view.page == 1
                           or f.view.screen == pb.SCREEN_MAP, timeout=5)
    assert frame.view.page_count >= 2


def pair_sensor(link, addr):
    """As the app's Sensors sheet does it: scan, see it, pair it."""
    link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_SCAN_START))
    link.wait_push("sensor_list", lambda l: any(s.address == addr for s in l.sensors), timeout=10)
    r = link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_PAIR, address=addr))
    link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_SCAN_STOP))
    assert any(s.address == addr and s.paired for s in r.sensor_list.sensors), r


def test_pairing_an_unseen_sensor_does_nothing(device, link):
    addr = device.sensor("hr", bpm=120, advertising=False)
    r = link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_PAIR, address=addr))
    assert not any(s.paired for s in r.sensor_list.sensors)


def test_live_frames_carry_a_virtual_heart_rate_strap(device, link):
    """A paired HR strap's beats reach the app's live RIDE topic."""
    addr = device.sensor("hr", bpm=142)
    pair_sensor(link, addr)
    device.wait_status(lambda s: s["ride"]["hr"] == 142, timeout=20)
    frame = link.wait_push("live", lambda f: f.ride.HasField("hr_bpm") and f.ride.hr_bpm == 142, timeout=10)
    assert frame.ride.show_offline   # the device's default: offline fields greyed


def test_a_dropped_sensor_goes_offline(device, link):
    addr = device.sensor("power", watts=250, cadence=90)
    pair_sensor(link, addr)
    device.wait_status(lambda s: s["ride"]["power"] == 250, timeout=20)
    device.cmd("sensor", kind="power", addr=addr, sending=False)   # stops transmitting
    st = device.wait_status(lambda s: s["ride"]["power"] == -1, timeout=40)
    frame = link.wait_push("live", lambda f: not f.ride.HasField("power_w"), timeout=10)
    assert not frame.ride.HasField("power_w")


def test_two_hellos_on_one_connection(link):
    """An app restart under iOS state restoration says Hello again."""
    link.drain_pushes()
    again = link.request(hello=pb.Hello(proto=pb.PROTO_VERSION_CURRENT, platform=pb.PLATFORM_IOS,
                                        app_version="again"))
    assert again.WhichOneof("body") == "hello"
    link.wait_push("settings", timeout=5)


def test_unknown_request_is_unsupported(link):
    r = link.request(agnss_info=pb.AgnssInfoRequest())
    assert r.WhichOneof("body") in ("agnss_info", "result")
