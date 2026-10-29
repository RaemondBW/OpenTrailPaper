"""Starred Strava segments: the set the app syncs (a TARGET_SEGMENTS upload of
segment_match.h's blob), kept on the card and timed live on the SEGMENT page.

tools/segment_test covers the blob format and the matcher on synthetic fixes
on the host. These run the device: the upload over the link, the copy on the
card and the reboot, the UI task adopting a new set, and the GPS model riding
through a segment while the page comes up by itself and goes back. What the
page shows is read through the control channel's `segments` command (the
matcher's own view, sim/host/tu/ui_dashboard.cpp)."""
import math
import os
import struct
import time

import pytest

from conftest import TIME_SCALE
from otpsim import LinkError, pb
from test_regressions_ui import offset

LAT0, LON0 = 37.77, -122.45
STARRED = os.path.join("segments", "starred.bin")


# -- the blob (segment_match.h; the apps' StravaSegments encoders) -------------------------------

def segment(sid, name, pts, pr_s=0, grade_d=0, elev=None):
    return dict(id=sid, name=name, pts=pts, pr_s=pr_s, grade_d=grade_d, elev=elev)


def blob(segs, version=2, magic=b"OTSG", count=None):
    out = bytearray(magic + struct.pack("<BBH", version, 0, len(segs) if count is None else count))
    for s in segs:
        name = s["name"].encode()
        dist_m = sum(math.hypot((b[0] - a[0]) * 110540, (b[1] - a[1]) * 111320 * math.cos(math.radians(a[0])))
                     for a, b in zip(s["pts"], s["pts"][1:]))
        out += struct.pack("<QIIhH", s["id"], s["pr_s"], int(dist_m * 10), s["grade_d"], len(s["pts"]))
        if version >= 2:
            out += struct.pack("<B", 1 if s["elev"] else 0)
        out += struct.pack("<B", len(name)) + name
        for i, (la, lo) in enumerate(s["pts"]):
            out += struct.pack("<ii", int(round(la * 1e5)), int(round(lo * 1e5)))
            if version >= 2:
                e = s["elev"][i] if s["elev"] else 0
                out += struct.pack("<H", int(round((e + 1000) * 10)))
    return bytes(out)


def east(m, north_m=0.0):
    return offset(LAT0, LON0, north_m, m)


def line(m0, m1, step=50, north_m=0.0):
    """A segment due east from m0 to m1 metres (west if m1 < m0)."""
    n = int(abs(m1 - m0) / step)
    return [east(m0 + (m1 - m0) * i / n, north_m) for i in range(n + 1)]


def send_set(link, segs, **kw):
    data = blob(segs, **kw)
    done = link.upload(pb.TARGET_SEGMENTS, "starred", data)
    assert done.code == pb.RESULT_OK
    return data


def ensure_segment_page(device, link):
    """The SEGMENT page (auto-show) as the last page of the rotation, as the
    app adds it; returns its page index."""
    text = device.status()["dash"]
    if "page segment" not in text:
        r = link.request(set_dash=pb.DashConfig(text=text.rstrip("\n") + "\npage segment\n"))
        assert r.WhichOneof("body") == "dash", r
        device.wait_status(lambda s: "page segment" in s["dash"], timeout=5)
    # Page 0 is the fields before the first `page` line; each `page` line
    # starts the next one (dash_layout.h).
    pages = [l for l in device.status()["dash"].splitlines() if l.startswith("page")]
    return 1 + [i for i, l in enumerate(pages) if l.startswith("page segment")][0]


def ride(device, pts, speed_kmh, until, timeout=240, factor=5):
    """Ride `pts` with the GPS model at `factor`x; poll the matcher until
    `until(segments, status)`; returns every (status, segments) seen."""
    device.gps(*pts[0], speed_kmh=0)
    device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    time.sleep(1.5)
    device.set_speed(factor)
    seen = []
    try:
        device.gps_track(pts, speed_kmh=speed_kmh)
        deadline = time.time() + timeout * TIME_SCALE / factor
        while time.time() < deadline:
            st, sg = device.status(), device.segments()
            seen.append((st, sg))
            if until(sg, st):
                return seen
            time.sleep(0.1)
    finally:
        device.set_speed(1)
    raise AssertionError("never happened; last: %s" % (seen[-1][1] if seen else None))


def wait_loaded(device, n, timeout=5):
    """The UI task adopts a new set at its next pass."""
    deadline = time.time() + timeout * TIME_SCALE
    while device.segments()["loaded"] != n:
        assert time.time() < deadline, device.segments()
        time.sleep(0.1)


def item(sg, name):
    return next((x for x in sg["list"] if x["name"] == name), None)


# -- the set: accepted, kept, refused ------------------------------------------------------------

def test_a_starred_set_is_kept_on_the_card_and_back_after_a_reboot(device, link):
    """The app syncs the rider's starred segments once; the device must still
    have them after a reboot (they are timed with no phone in the pocket):
    the card holds the blob as sent, GetSegments reports it, and the matcher
    has it again once the device is back up."""
    segs = [segment(1001, "Hawk Hill", line(0, 1000), pr_s=180),
            segment(1002, "Café sprint ⚡", line(2000, 2400), pr_s=40)]
    data = send_set(link, segs)
    assert open(os.path.join(device.sd, STARRED), "rb").read() == data
    info = link.request(get_segments=pb.GetSegments()).segments
    assert info.present and info.count == 2 and info.version >= 2, info
    wait_loaded(device, 2)
    device.reboot()
    sg = device.segments()
    assert sg["count"] == 2, sg
    wait_loaded(device, 2)
    device.gps(*east(-100), speed_kmh=0)
    device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    time.sleep(1.5)
    near = device.segments()["list"]
    assert near and near[0]["nearest"] == "Hawk Hill" and abs(near[0]["nearest_m"] - 100) < 10, near


@pytest.mark.parametrize("what,data", [
    ("bad magic", lambda segs: blob(segs, magic=b"OTSX")),
    ("too new a version", lambda segs: blob(segs, version=9)),
    ("cut short", lambda segs: blob(segs)[:-7]),
    ("a count past the data", lambda segs: blob(segs, count=3)),
    ("one point", lambda segs: blob([segment(9, "Dot", [east(0)])])),
])
def test_a_damaged_set_is_refused_and_the_one_on_the_device_stays(device, link, what, data):
    """A sync cut off half way, or from a newer app, must never replace a
    good set: RESULT_PARSE_ERROR, the card copy untouched, the matcher still
    on the old set."""
    segs = [segment(1001, "Hawk Hill", line(0, 1000), pr_s=180),
            segment(1002, "Sprint", line(2000, 2400), pr_s=40)]
    good = send_set(link, segs)
    wait_loaded(device, 2)
    with pytest.raises(LinkError) as e:
        link.upload(pb.TARGET_SEGMENTS, "starred", data(segs))
    assert e.value.code == "RESULT_PARSE_ERROR", (what, e.value)
    assert open(os.path.join(device.sd, STARRED), "rb").read() == good
    info = link.request(get_segments=pb.GetSegments()).segments
    assert info.count == 2, (what, info)
    time.sleep(1)
    assert device.segments()["loaded"] == 2


def test_a_new_set_replaces_the_old_one(device, link):
    """Un-starring segments in Strava and syncing again: the device has the
    new set only."""
    send_set(link, [segment(1, "A", line(0, 500)), segment(2, "B", line(600, 900))])
    send_set(link, [segment(3, "C", line(0, 700, north_m=500))])
    assert link.request(get_segments=pb.GetSegments()).segments.count == 1
    wait_loaded(device, 1)


# -- riding them -------------------------------------------------------------------------------

def test_riding_a_segment_times_it_start_line_to_finish_line(device, link):
    """A 1 km segment east, ridden at 36 km/h from 150 m before its start to
    150 m past its end. The effort starts at the start line and stops at the
    finish: 100 s (to a fix or two), whatever was ridden either side. The
    SEGMENT page comes up by itself as the effort starts and the panel goes
    back to the page the rider was on once the result has been shown."""
    slot = ensure_segment_page(device, link)
    send_set(link, [segment(7, "Hawk Hill", line(0, 1000), pr_s=150)])
    start_page = device.status()["page"]
    assert start_page != slot
    track = [east(x) for x in range(-150, 1151, 10)]
    seen = ride(device, track, 36, lambda sg, st: (item(sg, "Hawk Hill") or {}).get("phase") == "done")
    active = [(st, item(sg, "Hawk Hill")) for st, sg in seen if (item(sg, "Hawk Hill") or {}).get("phase") == "active"]
    assert active, "the effort never started"
    assert all(st["page"] == slot for st, _ in active[3:]), "the SEGMENT page (%d) didn't come up: %s %s" % (
        slot, sorted({(st["screen"], st["page"]) for st, _ in active}), device.status()["dash"])
    done = item(seen[-1][1], "Hawk Hill")
    assert abs(done["elapsed_ms"] - 100000) <= 2500, done
    assert done["new_pr"] and done["delta_ms"] is not None and done["delta_ms"] < -40000, done
    # The result shows for 30 s, then the panel goes back.
    device.set_speed(5)
    try:
        st = device.wait_status(lambda s: s["page"] == start_page, timeout=40 * TIME_SCALE / 5 + 5)
    finally:
        device.set_speed(1)
    assert st["screen"] in ("dash", "map")


def test_behind_pr_pace_the_live_delta_says_so_and_it_is_no_pr(device, link):
    """Against a PR of 80 s on a 1 km segment (45 km/h), a rider at 36 km/h is
    behind pace the whole way — the live delta is positive and growing — and
    the finish is not a new PR."""
    send_set(link, [segment(8, "Fast one", line(0, 1000), pr_s=80)])
    track = [east(x) for x in range(-100, 1101, 10)]
    seen = ride(device, track, 36, lambda sg, st: (item(sg, "Fast one") or {}).get("phase") == "done")
    deltas = [i["delta_ms"] for _, sg in seen for i in [item(sg, "Fast one")]
              if i and i["phase"] == "active" and i["done_m"] > 200 and i["delta_ms"] is not None]
    assert deltas and all(d > 0 for d in deltas), deltas
    assert deltas[-1] > deltas[0], deltas
    done = item(seen[-1][1], "Fast one")
    assert not done["new_pr"] and abs(done["elapsed_ms"] - 100000) <= 2500, done
    assert 15000 <= done["delta_ms"] <= 25000, done


def test_a_segment_ridden_the_wrong_way_is_not_timed(device, link):
    """Riding a one-way climb downhill passes both its ends; it is not an
    effort (the start arms only setting off along the first leg)."""
    send_set(link, [segment(9, "Climb", line(0, 800), pr_s=120)])
    track = [east(x) for x in range(950, -151, -10)]
    seen = ride(device, track, 36, lambda sg, st: st["gps"]["track_left"] == 0)
    phases = {i["phase"] for _, sg in seen for i in sg["list"]}
    assert "active" not in phases and "done" not in phases, phases


def test_leaving_a_segment_abandons_the_effort(device, link):
    """A rider who turns off half way and rides away is told "off segment"
    (abandoned), not left with a clock that never stops."""
    send_set(link, [segment(10, "Long drag", line(0, 1500), pr_s=200)])
    track = [east(x) for x in range(-100, 501, 10)] + [east(500, n) for n in range(10, 1200, 10)]
    seen = ride(device, track, 36, lambda sg, st: (item(sg, "Long drag") or {}).get("phase") == "abandoned",
                timeout=150)
    assert any((item(sg, "Long drag") or {}).get("phase") == "active" for _, sg in seen)
    assert not device.segments()["busy"]


def test_two_segments_that_overlap_are_both_timed(device, link):
    """Strava segments overlap (a climb, and the last 400 m of it). Both are
    timed on the one ride, each over its own span."""
    send_set(link, [segment(11, "Whole climb", line(0, 1000), pr_s=200),
                    segment(12, "Steep bit", line(600, 1000), pr_s=60)])
    track = [east(x) for x in range(-100, 1151, 10)]
    seen = ride(device, track, 36, lambda sg, st: all((item(sg, n) or {}).get("phase") == "done"
                                                      for n in ("Whole climb", "Steep bit")))
    a, b = item(seen[-1][1], "Whole climb"), item(seen[-1][1], "Steep bit")
    assert abs(a["elapsed_ms"] - 100000) <= 2500, a
    assert abs(b["elapsed_ms"] - 40000) <= 2500, b
