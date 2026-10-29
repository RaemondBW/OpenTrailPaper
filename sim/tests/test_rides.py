"""Riding: record with GPS and sensors, save on the panel, list and download
the FIT over the link — and the transfers around it (routes, firmware)."""
import math
import os
import struct
import time

import pytest

from conftest import TIME_SCALE, paired_phone
from otpsim import LinkClient, LinkError, SimDevice, pb

SAVE_BUTTON = (24 + 120, 700 + 48)   # ui_render.cpp kSaveButton centre


def line_track(lat, lon, km, bearing_deg=90, step_m=50):
    pts = []
    r = math.radians(bearing_deg)
    for i in range(int(km * 1000 / step_m) + 1):
        d = i * step_m
        pts.append((lat + d * math.cos(r) / 111320.0,
                    lon + d * math.sin(r) / (111320.0 * math.cos(math.radians(lat)))))
    return pts


def record_ride(device, seconds=90, speed_kmh=30):
    device.gps_track(line_track(37.77, -122.45, 3), speed_kmh=speed_kmh)
    device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    device.set_speed(6)
    device.button("boot")
    device.wait_status(lambda s: s["ride"]["recording"], timeout=10)
    device.wait_status(lambda s: s["ride"]["elapsed_s"] >= seconds, timeout=seconds / 3 + 20)
    device.button("boot")                     # stop: the save/discard summary
    device.wait_status(lambda s: s["screen"] == "other", timeout=10)
    device.tap(*SAVE_BUTTON)
    st = device.wait_status(lambda s: not s["ride"]["recording"], timeout=20)
    device.set_speed(1)
    return st


def check_fit(data):
    assert len(data) > 100
    hdr = data[0]
    assert hdr in (12, 14)
    assert data[8:12] == b".FIT"
    data_size = struct.unpack_from("<I", data, 4)[0]
    assert len(data) == hdr + data_size + 2   # header + records + file CRC


def test_record_save_list_and_download_a_ride(device, phone):
    link = LinkClient(phone).hello()
    hr = device.sensor("hr", bpm=150)
    link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_SCAN_START))
    link.wait_push("sensor_list", lambda l: any(s.address == hr for s in l.sensors), timeout=10)
    link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_PAIR, address=hr))
    link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_SCAN_STOP))
    device.wait_status(lambda s: s["ride"]["hr"] == 150, timeout=20)

    record_ride(device)
    pages = link.request(list=pb.ListRequest(kind=pb.LIST_RIDES), all_pages=True)
    rides = [f for p in pages for f in p.file_list.files]
    assert len(rides) == 1, rides
    ride = rides[0]
    assert ride.name.endswith(".fit") and ride.size > 0

    data = link.download(pb.SOURCE_RIDE, ride.name)
    assert len(data) == ride.size
    check_fit(data)
    assert open(os.path.join(device.sd, "rides", ride.name), "rb").read() == data


def test_a_ride_is_not_downloadable_while_recording(device, link):
    device.gps(37.77, -122.45)
    device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    device.button("boot")
    device.wait_status(lambda s: s["ride"]["recording"], timeout=10)
    with pytest.raises(LinkError) as e:
        link.download(pb.SOURCE_RIDE, "anything.fit")
    assert e.value.code in ("RESULT_RECORDING", "RESULT_NOT_FOUND")


@pytest.mark.xfail(strict=True, reason=(
    "BUG: ride_recorder::task (src/ride_recorder.cpp) counts the ride timer (timerS, and movingS/pausedS "
    "with it) once per loop, and paces the loop with vTaskDelay(RECORD_INTERVAL_MS): a second AFTER the "
    "tick's work, not a 1 s period (vTaskDelayUntil). The work — the FIT write under sdLock, waiting behind "
    "the map for the card — is time the timer never counts, so on a slow card the ride clock runs slow, "
    "records come at under 1 Hz and the average speed reads high"))
def test_the_ride_timer_keeps_time_on_a_slow_card(device):
    """A card that takes 100 ms an operation (a slow card, or one the map is
    busy with): the ride's elapsed time still counts every second ridden."""
    device.gps_track(line_track(37.77, -122.45, 3), speed_kmh=30)
    device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    device.button("boot")
    device.wait_status(lambda s: s["ride"]["recording"], timeout=10)
    device.set_sd_latency(100)
    device.set_speed(2)
    st0 = device.status()
    st = device.wait_status(lambda s: s["uptime_ms"] - st0["uptime_ms"] >= 40000, timeout=40 * TIME_SCALE)
    device.set_speed(1)
    device.set_sd_latency(0)
    ridden = (st["uptime_ms"] - st0["uptime_ms"]) / 1000.0
    counted = st["ride"]["elapsed_s"] - st0["ride"]["elapsed_s"]
    assert abs(counted - ridden) <= 1.5, "the ride timer counted %d s of %.1f s ridden" % (counted, ridden)


def test_download_survives_a_slow_link():
    """A slow connection interval with the stack's buffers full: the link task
    must wait out a busy notify() longer than one interval rather than give up
    (the ride that wouldn't download, 2026-10-25: 'notify failed' after 320 ms
    of retries on a 300 ms link)."""
    with SimDevice.start(args=["--gps-baud", "115200", "--conn-interval-ms", "400",
                               "--packets-per-event", "4", "--notify-window", "4"]) as dev:
        phone = paired_phone(dev)
        link = LinkClient(phone).hello(timeout=15)
        os.makedirs(os.path.join(dev.sd, "rides"), exist_ok=True)
        blob = os.urandom(24 * 1024)
        with open(os.path.join(dev.sd, "rides", "20260101-080000.fit"), "wb") as f:
            f.write(blob)
        data = link.download(pb.SOURCE_RIDE, "20260101-080000.fit", timeout=120)
        assert data == blob
        assert not [l for l in dev.log() if "notify failed" in l]


GPX = """<?xml version="1.0"?>
<gpx version="1.1" creator="sim"><trk><name>Loop</name><trkseg>
{}
</trkseg></trk></gpx>"""


def test_route_upload_and_maneuvers(device, link):
    pts = line_track(37.77, -122.45, 2)
    gpx = GPX.format("\n".join('<trkpt lat="%.6f" lon="%.6f"/>' % p for p in pts)).encode()
    done = link.upload(pb.TARGET_ROUTE, "Loop", gpx)
    assert done.code == pb.RESULT_OK
    turns = [pb.Maneuver(lat_e7=int(p[0] * 1e7), lon_e7=int(p[1] * 1e7), text="Continue")
             for p in pts[::10][:5]]
    link.expect_result(maneuvers=pb.RouteManeuvers(items=turns))
    link.expect_result(route_finish=pb.RouteFinish())
    device.wait_log(r"route|Route", since=0, timeout=5)
    assert os.listdir(os.path.join(device.sd, "routes"))


def test_a_bad_route_is_a_parse_error(device, link):
    with pytest.raises(LinkError) as e:
        link.upload(pb.TARGET_ROUTE, "Junk", b"not a gpx file at all")
    assert e.value.code == "RESULT_PARSE_ERROR"
    assert "track" in e.value.detail


def test_firmware_update_installs_and_reboots(device, phone):
    link = LinkClient(phone).hello()
    image = b"\xE9" + os.urandom(64 * 1024 - 1)
    done = link.upload(pb.TARGET_FIRMWARE, "firmware.bin", image, timeout=60)
    assert done.code == pb.RESULT_OK
    phone.wait_disconnected(15)
    time.sleep(1)
    device.reconnect_control(30)
    device.wait_log(r"boot: all tasks started", timeout=30, since=0)
    ota = os.path.join(device.dir, "ota.bin")
    deadline = time.time() + 30
    while not (os.path.exists(ota) and os.path.getsize(ota) == len(image)) and time.time() < deadline:
        time.sleep(0.2)
    assert open(ota, "rb").read() == image


def test_upload_with_a_bad_crc_is_refused(device, link):
    import zlib
    data = os.urandom(4000)
    orig = zlib.crc32
    try:
        zlib.crc32 = lambda d, *a: orig(d, *a) ^ 1
        with pytest.raises(LinkError) as e:
            link.upload(pb.TARGET_WORKOUT, "x.erg", data)
    finally:
        zlib.crc32 = orig
    assert e.value.code == "RESULT_CRC_MISMATCH"
