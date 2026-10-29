"""GPS and clock bugs that shipped once, replayed against the sim.

Each test names the commit that fixed its bug, and was checked against the
unfixed behaviour re-created in src/: it fails with the bug and passes with
the fix."""
import math
import time

import pytest

from otpsim import LinkClient, SimDevice, pb

LAT, LON = 37.7749, -122.4194

# Indoors after a reboot: the receiver has no fix and no UTC either.
indoors = pytest.mark.parametrize("device", [["--gps-baud", "115200", "--gps-cold"]], indirect=True,
                                  ids=["indoors"])


def phone_fix(link, lat, lon, utc=None, acc_m=10, speed_cms=None, course_cdeg=None):
    """One location from the app's stream (link PhoneLocation)."""
    loc = pb.PhoneLocation(lat_e7=int(lat * 1e7), lon_e7=int(lon * 1e7), accuracy_m=acc_m)
    if utc is not None:
        loc.utc = int(utc)
    if speed_cms is not None:
        loc.speed_cms = speed_cms
    if course_cdeg is not None:
        loc.course_cdeg = course_cdeg
    link.tell(location=loc)


@pytest.mark.parametrize("module_baud", [38400, 57600])
def test_an_l76k_left_at_another_baud_still_gets_a_fix(module_baud):
    """24ba88c: an L76K left at a non-default baud (kept by its backup supply)
    never answered the PCAS06 probe, and any byte read at 38400 passed as a
    u-blox M10Q: the unit reported a u-blox, never moved the module and never
    parsed a sentence, so the rider never got a fix."""
    with SimDevice.start(args=["--gps-baud", str(module_baud)]) as dev:
        dev.gps(LAT, LON)
        st = dev.wait_status(lambda s: s["ride"]["gps_fix"], timeout=40)
        assert abs(st["ride"]["lat"] - LAT) < 1e-4
        assert not [l for l in dev.log() if "u-blox" in l]


def test_speed_drops_to_zero_when_the_fix_is_lost_or_the_receiver_goes_quiet(device):
    """d88990d (#99): TinyGPS++ never clears isValid(), so the last moving
    speed stayed on the dashboard (and in moving time / auto-pause) after the
    fix was lost; and fix and speed were only re-evaluated on a new sentence,
    so a receiver that went silent left 'fix, 30 km/h' frozen forever."""
    device.gps(LAT, LON, speed_kmh=30, course=90)
    device.wait_status(lambda s: s["ride"]["gps_fix"] and s["ride"]["speed_kmh"] > 25, timeout=10)

    # The sky goes away: RMC status V, GGA without a position.
    device.gps(LAT, LON, speed_kmh=30, fix=False, sats=3)
    device.wait_status(lambda s: not s["ride"]["gps_fix"] and s["ride"]["speed_kmh"] == 0, timeout=8)

    device.gps(LAT, LON, speed_kmh=30, course=90)
    device.wait_status(lambda s: s["ride"]["gps_fix"] and s["ride"]["speed_kmh"] > 25, timeout=10)

    # The receiver stops talking altogether.
    device.gps_silent(True)
    device.wait_status(lambda s: not s["ride"]["gps_fix"] and s["ride"]["speed_kmh"] == 0, timeout=8)


@indoors
def test_the_phone_location_sets_the_clock_without_a_gps_fix(device, phone):
    """f428896: indoors after a reboot the status bar and CLOCK field showed
    --:-- until GPS produced a time, although the phone sends UTC with every
    location. The clock now comes from the phone and keeps ticking."""
    link = LinkClient(phone).hello(unix_ms=0)   # an app that doesn't send its clock in Hello
    time.sleep(1.5)
    assert not device.status()["ride"]["time_valid"]
    now = time.time()
    phone_fix(link, LAT, LON, utc=now)
    st = device.wait_status(lambda s: s["ride"]["time_valid"], timeout=5)
    assert abs(st["wall_unix"] - now) < 5, st["wall_unix"]
    time.sleep(2.5)
    st = device.status()
    assert st["ride"]["time_valid"]
    assert abs(st["wall_unix"] - time.time()) < 5


@indoors
def test_hello_sets_the_clock_without_a_location_stream(device, phone):
    """3e93c8f: a phone that connects but doesn't stream locations (no
    permission, or the device already has a fix-less receiver and the app
    holds back) left the clock at --:--. The phone's clock now comes in
    Hello on every connection."""
    time.sleep(1.5)
    assert not device.status()["ride"]["time_valid"]
    now = time.time()
    LinkClient(phone).hello(unix_ms=now * 1000)
    st = device.wait_status(lambda s: s["ride"]["time_valid"], timeout=5)
    assert abs(st["wall_unix"] - now) < 5, st["wall_unix"]


def test_the_live_position_follows_the_phone_before_any_fix(device, link):
    """b8adbc1: the app's map page drew the device's rider marker only once
    the device had ever had a GPS fix; indoors after a reboot the live frames
    carried no position although the device map was centred on the phone."""
    link.expect_result(live_subscribe=pb.LiveSubscribe(topics=[pb.LIVE_TOPIC_POSITION]))
    phone_fix(link, LAT, LON, utc=time.time())
    frame = link.wait_push("live", lambda f: f.HasField("position") and f.position.lat_e7 != 0, timeout=5)
    assert abs(frame.position.lat_e7 / 1e7 - LAT) < 1e-5
    assert abs(frame.position.lon_e7 / 1e7 - LON) < 1e-5
    assert frame.position.from_phone and frame.position.current


def test_phone_speed_is_shown_when_the_device_has_no_fix(device, link):
    """d9ae99d (#103): with the phone as the position source, the speed sat
    at 0 (and the heading at due north): nothing wrote the phone's motion
    into the ride state."""
    for i in range(3):
        phone_fix(link, LAT, LON + i * 0.0001, utc=time.time(), speed_cms=800, course_cdeg=9000)
        time.sleep(1)
    st = device.wait_status(lambda s: abs(s["ride"]["speed_kmh"] - 28.8) < 1, timeout=5)
    assert not st["ride"]["gps_fix"]


def test_a_ride_records_the_phone_position_while_the_receiver_has_none(device, link):
    """363940b: the recorder only wrote a point inside `if (s.gpsFix)`, so
    with a cold receiver — the opening minutes of a ride, phone in the
    pocket — the ride had no track and no distance."""
    device.button("boot")
    device.wait_status(lambda s: s["ride"]["recording"], timeout=10)
    lat, lon = LAT, LON
    step = 8.0   # metres a second, ~29 km/h, due north
    for _ in range(12):
        phone_fix(link, lat, lon, utc=time.time(), speed_cms=800, course_cdeg=0)
        lat += step / 111320.0
        time.sleep(1)
    st = device.wait_status(lambda s: s["ride"]["distance_m"] > 40, timeout=5)
    assert not st["ride"]["gps_fix"]
    assert st["ride"]["distance_m"] < 12 * step * 1.5
