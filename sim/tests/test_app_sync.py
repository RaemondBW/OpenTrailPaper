"""The device's side of the apps' ride sync: what the Rides tab relies on
the link to do when the rider's card is big, the link is slow or turns slow,
a download is resumed, or a ride is deleted mid-ride. The apps' own end of
these flows runs against the sim in the Android (RideSyncSimTest,
LinkEdgeSimTest) and iOS (FirmwareSimAppFlowTests) suites."""
import os
import subprocess
import threading
import time

import pytest

from conftest import TIME_SCALE, paired_phone
from otpsim import LinkClient, LinkError, SimDevice, pb
from otpsim.device import BINARY


def card_rides(device, specs):
    """Finished rides on the card from the firmware's FitWriter (card-tool fit):
    specs are (start unix, seconds, km/h, hr, watts). Returns the names."""
    names, lines = [], []
    for start, secs, kmh, hr, watts in specs:
        name = time.strftime("%Y%m%d-%H%M%S", time.gmtime(start)) + ".fit"
        names.append(name)
        lines.append("/rides/%s %d %d 45.70000 -121.52000 60 %.1f %d %d" % (name, start, secs, kmh, hr, watts))
    r = subprocess.run([BINARY, "card-tool", "fit", device.sd], input="\n".join(lines) + "\n",
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    return names


def ride_list(link):
    pages = link.request(list=pb.ListRequest(kind=pb.LIST_RIDES), all_pages=True, timeout=30 * TIME_SCALE)
    return [(f.name, f.size) for p in pages for f in p.file_list.files], len(pages)


def test_a_card_of_six_hundred_rides_lists_whole_over_a_slow_link(device, link):
    """Years of rides on the card, and the link slow (300 ms interval, four
    packets an event, the controller's buffers nearly full): every page of
    the list still arrives, newest first, each ride with its size — a page
    the device failed to notify would leave the app's list short."""
    names = card_rides(device, [(1_700_000_000 + i * 86_400 + (i % 5) * 3_600, 30, 22, 0, 0) for i in range(600)])
    device.link_pace(interval_ms=300, packets=4, window=4)
    t0 = time.time()
    rides, pages = ride_list(link)
    took = time.time() - t0
    assert [n for n, _ in rides] == sorted(names, reverse=True)
    for n, size in rides:
        assert size == os.path.getsize(os.path.join(device.sd, "rides", n)), n
    assert pages >= 600 // 16
    assert not [l for l in device.log() if "notify failed" in l]
    print("600 rides in %d pages over the slow link: %.1f s" % (pages, took))


def test_a_link_that_turns_slow_mid_download_still_delivers_the_ride_whole(device, link):
    """The connection interval stretches in the middle of a ride's download
    (the phone in a pocket, a busy radio): the download slows but finishes
    with every byte, and speeds up again when the link does."""
    name = card_rides(device, [(1_760_000_000, 3 * 3_600, 25, 140, 200)])[0]
    want = open(os.path.join(device.sd, "rides", name), "rb").read()
    assert len(want) > 200_000
    out = {}

    def fetch():
        try:
            out["data"] = link.download(pb.SOURCE_RIDE, name, timeout=120 * TIME_SCALE)
        except Exception as e:   # noqa: BLE001 - reported below
            out["error"] = e

    t = threading.Thread(target=fetch)
    t0 = time.time()
    t.start()
    time.sleep(0.05)
    paced = device.link_pace(interval_ms=60, packets=2, window=4)
    assert (paced["interval_ms"], paced["packets"], paced["window"]) == (60, 2, 4)
    time.sleep(2)
    device.link_pace(interval_ms=0, packets=0, window=32)
    t.join(150 * TIME_SCALE)
    assert "error" not in out, out.get("error")
    assert out["data"] == want
    print("%d bytes with 2 s of a slow link in the middle: %.1f s" % (len(want), time.time() - t0))


def test_a_resumed_download_starts_where_the_phone_stopped_and_checks_the_whole_file(device, link):
    """The apps keep the bytes of an interrupted download and ask for the
    rest: the device starts at that offset and its TransferDone CRC covers
    the whole file, so the phone's prefix is checked too. A prefix that
    doesn't match the ride (the file changed in between) fails the CRC — the
    app must then drop it — rather than passing as the ride."""
    name = card_rides(device, [(1_760_000_000, 3_600, 25, 140, 200)])[0]
    want = open(os.path.join(device.sd, "rides", name), "rb").read()
    cut = len(want) // 3

    data = link.download(pb.SOURCE_RIDE, name, have=want[:cut])
    assert link.last_download_begin.offset == cut
    assert link.last_download_begin.size == len(want)
    assert data == want

    stale = bytes(b ^ 0x5A for b in want[:cut])
    with pytest.raises(LinkError) as e:
        link.download(pb.SOURCE_RIDE, name, have=stale)
    assert e.value.code == "CRC_MISMATCH"
    assert link.last_download_begin.offset == cut, "the device resumed where asked"

    # The whole file is the prefix: nothing left to send, still checked.
    assert link.download(pb.SOURCE_RIDE, name, have=want) == want
    assert link.last_download_begin.offset == len(want)


def test_a_ride_deleted_while_recording_is_refused_and_kept(device, link):
    """The card is the recorder's while a ride records: the device answers a
    delete with RESULT_RECORDING and keeps the ride (the apps must tell the
    rider, not drop it from their list — see RideSyncSimTest
    aRideDeletedWhileTheDeviceRecordsIsNotSilentlyLost); once the ride is
    saved, the same delete goes through."""
    name = card_rides(device, [(1_760_000_000, 600, 25, 0, 0)])[0]
    path = os.path.join(device.sd, "rides", name)
    device.gps(45.70, -121.52, speed_kmh=20)
    device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10 * TIME_SCALE)
    device.button("boot")
    device.wait_status(lambda s: s["ride"]["recording"], timeout=10 * TIME_SCALE)
    r = link.request(delete_file=pb.DeleteRequest(kind=pb.LIST_RIDES, name=name))
    assert r.result.code == pb.RESULT_RECORDING
    assert os.path.exists(path)
    r = link.request(list=pb.ListRequest(kind=pb.LIST_RIDES))
    assert r.WhichOneof("body") == "result" and r.result.code == pb.RESULT_RECORDING

    device.button("boot")
    device.wait_status(lambda s: s["screen"] == "other", timeout=10 * TIME_SCALE)
    device.tap(24 + 120, 700 + 48)   # SAVE
    device.wait_status(lambda s: not s["ride"]["recording"], timeout=20 * TIME_SCALE)
    r = link.request(delete_file=pb.DeleteRequest(kind=pb.LIST_RIDES, name=name))
    assert r.result.code == pb.RESULT_OK
    assert not os.path.exists(path)
    rides, _ = ride_list(link)
    assert [n for n, _ in rides if n == name] == []
    assert len(rides) == 1, "the ride just recorded is listed"
