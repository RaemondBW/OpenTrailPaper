"""Routes and turn-by-turn navigation: what the app sends, what the device
keeps on its card, what it shows a rider following a route.

Routes arrive over the link as a GPX upload (routes.cpp loadFromMemory: parsed
and written to /routes/<name>), followed by the turn cues (RouteManeuvers) and
RouteFinish, which raises the NAVIGATE prompt. The same files can be picked
from the device's own NAVIGATE list (routes.cpp load: streamed from the card),
which is also how a rider rides a .gpx copied onto the card over USB.

The rider is the GPS module (`gps` / `gps track`); what the turn banner says
is the status JSON's `nav` (the same routes:: calls the banner draws from)."""
import math
import os
import re
import time

import pytest

from conftest import TIME_SCALE
from otpsim import LinkError, pb
from test_regressions_ui import POWER_SHUTDOWN, accept_route, glass, line_track, offset, send_route

W, H = 540, 960
LAT0, LON0 = 37.77, -122.45

MENU_NAVIGATE = (270, 64 + 1 * 148 + 74)      # Menu row 1, NAVIGATE


def row_xy(r):
    """Centre of row `r` of a menu-style list (kMenuRowTop 64, ROW_H 148)."""
    return (270, 64 + r * 148 + 74)


def along(m, north_m=0.0):
    """A point `m` metres east of LAT0/LON0 (the routes below run east)."""
    return offset(LAT0, LON0, north_m, m)


def trkpts(pts, fmt='<trkpt lat="%.7f" lon="%.7f"/>'):
    return "\n".join(fmt % p for p in pts)


def gpx(pts, name="Test", before_trk="", fmt='<trkpt lat="%.7f" lon="%.7f"/>'):
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<gpx version="1.1" creator="sim">%s'
            '<trk><name>%s</name><trkseg>\n%s\n</trkseg></trk></gpx>\n'
            % (before_trk, name, trkpts(pts, fmt))).encode()


# Metres per degree on the firmware's sphere (routes.cpp haversineM, R = 6371 km): the
# distances it reports are on this scale, 0.11% short of offset()'s 111320.
M_PER_DEG = 6371000.0 * math.pi / 180


def length_m(pts):
    tot = 0.0
    for (a, b), (c, d) in zip(pts, pts[1:]):
        dy = (c - a) * M_PER_DEG
        dx = (d - b) * M_PER_DEG * math.cos(math.radians(a))
        tot += math.hypot(dx, dy)
    return tot


ROUTE_LINE = re.compile(r"\[route\] (?:uploaded )?(.+?): (\d+) pts, ([\d.]+) km")


def route_lines(device, since=0):
    """Every `[route] <name>: N pts, K km` the device printed (load and upload)."""
    out = []
    for l in device.log(since):
        m = ROUTE_LINE.search(l)
        if m:
            out.append((m.group(1), int(m.group(2)), float(m.group(3)), "uploaded" in l))
    return out


def open_navigate(device):
    """The device's own NAVIGATE list, from the menu."""
    device.console("screen menu")
    device.wait_status(lambda s: s["screen"] == "other", timeout=5 * TIME_SCALE)
    time.sleep(0.5)
    device.tap(*MENU_NAVIGATE)
    time.sleep(1.0)


def pick_route(device, row):
    """Tap row `row` of the NAVIGATE list (row 0 is "Clear route" while a
    route is loaded); returns the `[route]` line the load printed."""
    seq = device.log_seq()
    device.tap(*row_xy(row))
    line = device.wait_log(r"\[route\] ", timeout=5 * TIME_SCALE, since=seq)
    m = ROUTE_LINE.search(line)
    return (m.group(1), int(m.group(2)), float(m.group(3))) if m else line


def upload(link, name, data):
    return link.upload(pb.TARGET_ROUTE, name, data)


# -- storage: what the app sends is what the card keeps ----------------------------------------

def test_an_uploaded_route_is_kept_byte_for_byte_and_listed(device, link):
    """The app's GPX is written to /routes/<name> as sent — the card copy is
    what the device rides after a reboot and what the app lists back."""
    pts = line_track(LAT0, LON0, 3, step_m=25)
    data = gpx(pts, "Coast")
    assert upload(link, "coast.gpx", data).code == pb.RESULT_OK
    assert open(os.path.join(device.sd, "routes", "coast.gpx"), "rb").read() == data
    files = [f for p in link.request(list=pb.ListRequest(kind=pb.LIST_ROUTES), all_pages=True)
             for f in p.file_list.files]
    assert [(f.name, f.size) for f in files] == [("coast.gpx", len(data))]
    name, n, km, up = route_lines(device)[-1]
    assert (name, n, up) == ("coast.gpx", len(pts), True)
    assert abs(km - length_m(pts) / 1000) < 0.02
    st = device.status()
    assert st["nav"]["route"] and abs(st["nav"]["remaining_km"] - length_m(pts) / 1000) < 0.02


@pytest.mark.xfail(strict=True, reason=(
    "BUG: routes.cpp load() re-scans the 64-byte tail it carries over between 2 KB chunks, so "
    "attributes already parsed are parsed again: at every chunk boundary the route gains a "
    "duplicate point or a bogus one (a point's lat paired with the previous point's lon). A route "
    "picked from NAVIGATE has ~1 extra point per 2 KB (246 vs 241 for a 10 KB GPX) and zig-zags off "
    "its line there (6.013 km vs 5.993 km); on a sparse polyline the zig is a whole point spacing."))
def test_a_route_reloaded_from_the_card_is_the_route_that_was_sent(device, link):
    """After a reboot the route is no longer loaded (nothing restores it); the
    rider picks it from NAVIGATE, which streams the card copy through
    routes.cpp load() in 2 KB chunks. It must come back as the same route the
    upload parsed: same points, same length."""
    pts = line_track(LAT0, LON0, 6, bearing_deg=45, step_m=25)   # ~10 KB: five chunk boundaries
    data = gpx(pts, "Long")
    assert upload(link, "long.gpx", data).code == pb.RESULT_OK
    _, n_up, km_up, _ = route_lines(device)[-1]
    left_up = device.status()["nav"]["remaining_km"]
    device.reboot()
    assert not device.status()["nav"]["route"]
    open_navigate(device)
    name, n, km = pick_route(device, 0)
    assert name == "long.gpx"
    left = device.status()["nav"]["remaining_km"]
    assert (n, round(left * 1000)) == (n_up, round(left_up * 1000)), (
        "uploaded: %d pts %.3f km; from the card: %d pts %.3f km" % (n_up, left_up, n, left))


def test_sending_a_route_again_under_the_same_name_replaces_it(device, link):
    """The app re-sends a route after the rider edits it, under the same file
    name. The card must hold the new file only — a long old file under a
    short new one must not leave its tail behind — and the device must be on
    the new route."""
    long_pts = line_track(LAT0, LON0, 8, step_m=25)
    short_pts = line_track(LAT0, LON0, 2, bearing_deg=0, step_m=25)
    assert upload(link, "loop.gpx", gpx(long_pts, "Loop")).code == pb.RESULT_OK
    new = gpx(short_pts, "Loop")
    assert upload(link, "loop.gpx", new).code == pb.RESULT_OK
    assert open(os.path.join(device.sd, "routes", "loop.gpx"), "rb").read() == new
    files = [f for p in link.request(list=pb.ListRequest(kind=pb.LIST_ROUTES), all_pages=True)
             for f in p.file_list.files]
    assert [(f.name, f.size) for f in files] == [("loop.gpx", len(new))]
    assert abs(device.status()["nav"]["remaining_km"] - length_m(short_pts) / 1000) < 0.02


def test_a_route_name_in_any_script_is_kept_listed_and_loadable(device, link):
    """Route names come from the rider's destination or the file they
    imported. A short UTF-8 name is stored, listed back to the app and
    loadable from NAVIGATE under exactly those bytes."""
    name = "café_côte.gpx"                         # 15 bytes of UTF-8
    pts = line_track(LAT0, LON0, 1, step_m=25)
    assert upload(link, name, gpx(pts, "Café")).code == pb.RESULT_OK
    assert os.path.exists(os.path.join(device.sd, "routes", name))
    files = [f.name for p in link.request(list=pb.ListRequest(kind=pb.LIST_ROUTES), all_pages=True)
             for f in p.file_list.files]
    assert files == [name]
    device.reboot()
    open_navigate(device)
    assert pick_route(device, 0)[0] == name


@pytest.mark.xfail(strict=True, reason=(
    "BUG: link.cpp TARGET_ROUTE commit copies the name into char[routes::MAX_NAME + 8] (40 bytes) "
    "with copyStr, silently cutting a longer one: 'москва_красная_площадь.gpx' (48 B) is stored as "
    "'москва_красная_площа' with no .gpx, which routes::list() skips, so after a reboot the route "
    "is on the card but never in the device's NAVIGATE list (and routes.cpp curName[32] cuts the "
    "active name again, so deleting it from the app doesn't unload it). iOS RouteView.fileName "
    "allows 24 letters of any script, so the app does send such names."))
def test_a_route_name_longer_than_the_device_keeps_is_not_cut_silently(device, link):
    """iOS names a route after its destination: up to 24 letters, any script
    (RouteView.fileName keeps every `isLetter`), so a Cyrillic or Greek place
    name is 40-50 bytes. The device keeps route names in 32 bytes. Whatever it
    does with a longer name, the route must still be one the rider can find and
    pick on the device later (a .gpx in its NAVIGATE list) — or the upload
    must be refused so the app can say so."""
    name = "москва_красная_площадь.gpx"              # 26 characters, 48 bytes
    pts = line_track(LAT0, LON0, 1, step_m=25)
    try:
        done = upload(link, name, gpx(pts, "Moscow"))
    except LinkError:
        return                                     # refused: the app tells the rider
    assert done.code == pb.RESULT_OK
    stored = os.listdir(os.path.join(device.sd, "routes"))
    assert len(stored) == 1 and stored[0].endswith(".gpx"), "stored as %r" % stored
    device.reboot()
    open_navigate(device)
    picked = pick_route(device, 0)
    assert isinstance(picked, tuple) and picked[0] == stored[0], picked


@pytest.mark.xfail(strict=True, reason=(
    "BUG: routes.cpp list() copies each name into char[routes::MAX_NAME] (32 B), so a .gpx with a "
    "longer name is listed cut short in NAVIGATE and tapping it fails ('[route] can't open "
    "/routes/2026-10-28 Morning coast loop.g'): nothing loads, no message on the panel."))
def test_a_long_file_name_copied_onto_the_card_can_be_ridden(device):
    """A rider copies a .gpx onto the card over USB under whatever name the
    planner exported — FAT keeps long names, and 33+ characters is ordinary
    ("2026-10-28 Morning loop to the coast.gpx"). It must be listed whole and
    load when tapped."""
    name = "2026-10-28 Morning coast loop.gpx"         # 33 bytes
    pts = line_track(LAT0, LON0, 2, step_m=25)
    device.usb_host(True)
    with open(os.path.join(device.sd, "routes", name), "wb") as f:
        f.write(gpx(pts, "Morning"))
    device.usb_host(False)
    open_navigate(device)
    picked = pick_route(device, 0)
    assert device.status()["nav"]["route"], "tapping %r loaded nothing: %r" % (name, picked)


def test_a_route_the_device_cannot_read_leaves_nothing_behind(device, link):
    """A GPX with no track (one point, or none) is refused with
    RESULT_PARSE_ERROR, so the app can say "couldn't read the route" — and
    nothing is written to the card, listed, or drawn."""
    for i, data in enumerate([
            gpx([along(0)]),                                          # one point
            b'<?xml version="1.0"?><gpx><trk><trkseg></trkseg></trk></gpx>',
            gpx(line_track(LAT0, LON0, 1))[:120],                    # cut off after one lat=
            b"<html><body>404 Not Found</body></html>" + b" " * 64]):
        with pytest.raises(LinkError) as e:
            upload(link, "bad%d.gpx" % i, data)
        assert e.value.code == "RESULT_PARSE_ERROR", (i, e.value)
    assert os.listdir(os.path.join(device.sd, "routes")) == []
    st = device.status()["nav"]
    assert not st["route"] and not st["pending"], st


def test_a_route_over_the_size_limit_is_refused_up_front(device, link):
    """The device holds a route upload in 256 KB (DeviceHello
    route_max_bytes). A bigger one is refused at UploadBegin, before a byte is
    sent, as RESULT_TOO_LARGE."""
    hello_max = 256 * 1024
    data = b" " * (hello_max + 1)
    with pytest.raises(LinkError) as e:
        upload(link, "huge.gpx", data)
    assert e.value.code == "RESULT_TOO_LARGE", e.value
    assert os.listdir(os.path.join(device.sd, "routes")) == []


def test_a_route_name_with_a_path_in_it_is_refused(device, link):
    """The name becomes /routes/<name> on the card: a '/' in it would write
    somewhere else. Refused as RESULT_BAD_REQUEST."""
    pts = line_track(LAT0, LON0, 1)
    with pytest.raises(LinkError) as e:
        upload(link, "../settings.gpx", gpx(pts))
    assert e.value.code == "RESULT_BAD_REQUEST", e.value
    assert not os.path.exists(os.path.join(device.sd, "settings.gpx"))


def test_deleting_the_route_being_navigated_ends_the_navigation(device, link):
    """The app's route list can delete the route the rider is following. The
    file goes, and so does the route on the map and the turn banner — a
    banner for a route that no longer exists anywhere would be a lie."""
    pts = line_track(LAT0, LON0, 2, step_m=25)
    device.gps(*along(100), speed_kmh=12, course=90)
    device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    send_route(link, "ride.gpx", pts, [along(1000) + ("Turn left",)])
    accept_route(device)
    assert device.status()["nav"]["next"] == "Turn left"
    link.expect_result(delete_file=pb.DeleteRequest(kind=pb.LIST_ROUTES, name="ride.gpx"))
    st = device.wait_status(lambda s: not s["nav"]["route"], timeout=5 * TIME_SCALE)
    assert not st["nav"]["active"] and st["nav"]["next"] is None, st["nav"]
    assert os.listdir(os.path.join(device.sd, "routes")) == []
    with pytest.raises(LinkError) as e:
        link.expect_result(delete_file=pb.DeleteRequest(kind=pb.LIST_ROUTES, name="ride.gpx"))
    assert e.value.code == "RESULT_NOT_FOUND"


# -- following a route ---------------------------------------------------------------------------

def polyline(*legs, step_m=20):
    """A route from LAT0/LON0 along `legs` [(bearing_deg, metres)], a point
    every `step_m`; returns (points, corner along-distances)."""
    pts, corners = [(LAT0, LON0)], []
    done = 0.0
    for bearing, metres in legs:
        r = math.radians(bearing)
        la, lo = pts[-1]
        n = int(round(metres / step_m))
        for i in range(1, n + 1):
            d = metres * i / n
            pts.append(offset(la, lo, d * math.cos(r), d * math.sin(r)))
        done += metres
        corners.append(done)
    return pts, corners


def at_along(pts, m):
    """The point `m` metres along the polyline `pts`."""
    for a, b in zip(pts, pts[1:]):
        seg = length_m([a, b])
        if m <= seg:
            f = m / seg if seg else 0
            return (a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f)
        m -= seg
    return pts[-1]


def along_of(pts, lat, lon):
    """Where (lat, lon) projects onto the polyline, in metres along it."""
    best, best_m, done = 1e18, 0.0, 0.0
    cosl = math.cos(math.radians(lat))
    for a, b in zip(pts, pts[1:]):
        ax, ay = (a[1] - lon) * M_PER_DEG * cosl, (a[0] - lat) * M_PER_DEG
        bx, by = (b[1] - lon) * M_PER_DEG * cosl, (b[0] - lat) * M_PER_DEG
        vx, vy = bx - ax, by - ay
        l2 = vx * vx + vy * vy
        t = max(0.0, min(1.0, -(ax * vx + ay * vy) / l2)) if l2 else 0.0
        d2 = (ax + vx * t) ** 2 + (ay + vy * t) ** 2
        seg = length_m([a, b])
        if d2 < best:
            best, best_m = d2, done + t * seg
        done += seg
    return best_m


def navigate(device, link, pts, turns, start=None, name="test.gpx"):
    """Put the rider at `start` (default: the route's first point), send the
    route with `turns` [(metres along, text)] and accept the NAVIGATE prompt."""
    la, lo = start or pts[0]
    device.gps(la, lo, speed_kmh=0)
    device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    send_route(link, name, pts, [at_along(pts, m) + (t,) for m, t in turns])
    # A rider already past the route's first point has the prompt grabbed
    # away within a second (see test_a_rider_standing_on_the_route_keeps_the_
    # prompt_until_they_answer): START only if it is still up.
    st = device.wait_status(lambda s: s["nav"]["pending"] or s["nav"]["active"], timeout=10)
    if st["nav"]["active"]:
        return st
    time.sleep(0.9)                           # taps are ignored for 700 ms after it appears
    if device.status()["nav"]["pending"]:
        device.tap(*POWER_SHUTDOWN)           # START
    return device.wait_status(lambda s: s["nav"]["active"], timeout=10)


def hold(device, lat, lon, settle_s=1.3):
    """The rider stands at (lat, lon) until the firmware has projected it."""
    device.gps(lat, lon, speed_kmh=0)
    device.wait_status(lambda s: abs(s["ride"]["lat"] - lat) < 2e-6 and abs(s["ride"]["lon"] - lon) < 2e-6,
                       timeout=5 * TIME_SCALE)
    time.sleep(settle_s)
    return device.status()["nav"]


def test_riding_a_route_counts_down_to_each_turn_in_order(device, link):
    """A 1.4 km route with three corners, ridden at 36 km/h by the GPS
    model. Every turn is announced in order, none skipped; the distance to it
    is the real one along the route from where the rider is (within one fix's
    travel) and only ever counts down; each hands over to the next once the
    rider is past it — not before the corner, and within 35 m after."""
    pts, corners = polyline((90, 400), (0, 300), (90, 400), (180, 300))
    turns = [(corners[0], "Turn left onto Oak St"), (corners[1], "Turn right onto 3rd Ave"),
             (corners[2], "Turn right onto Pine St"), (corners[3], "Arrive at destination")]
    navigate(device, link, pts, turns)
    device.set_speed(5)
    device.gps_track(pts, speed_kmh=36)
    seen = []                                   # (along, next, next_m)
    deadline = time.time() + 60 * TIME_SCALE
    while time.time() < deadline:
        st = device.status()
        nav, ride = st["nav"], st["ride"]
        seen.append((along_of(pts, ride["lat"], ride["lon"]), nav["next"], nav["next_m"]))
        if st["gps"].get("track_left", 1) == 0 and seen[-1][0] > corners[3] - 1:
            break
        time.sleep(0.05)
    device.set_speed(1)
    names = []
    for _, t, _ in seen:
        if not names or names[-1] != t:
            names.append(t)
    assert names == [t for _, t in turns], "cues in order: %s" % names
    for (m, text), nxt in zip(turns, turns[1:] + [(None, None)]):
        mine = [(a, d) for a, t, d in seen if t == text]
        for a, d in mine:
            assert abs(d - max(0.0, m - a)) < 12, "%s: at %.0f m the banner said %.0f m (truth %.0f)" % (
                text, a, d, m - a)
        ds = [d for _, d in mine]
        assert all(b <= a + 1 for a, b in zip(ds, ds[1:])), "%s: the countdown went up: %s" % (text, ds)
        if nxt[1]:
            handover = min(a for a, t, _ in seen if t == nxt[1])
            # Past by 20 m (routes.cpp kManeuverPastM), seen at the next fix (10 m on).
            assert m - 10 <= handover <= m + 35, "%s at %.0f m handed over at %.0f m" % (text, m, handover)


# An out-and-back: 1 km east on the north side of the road, a U-turn, and back
# west on the south side (8 m apart, the two carriageways of a road).
OUT_M, LANES_M = 1000.0, 8.0


def out_and_back():
    out = [offset(LAT0, LON0, LANES_M / 2, x) for x in range(0, int(OUT_M) + 1, 20)]
    back = [offset(LAT0, LON0, -LANES_M / 2, x) for x in range(int(OUT_M), -1, -20)]
    pts = out + back
    turns = [(600, "Keep left at the fork"),                    # outbound, 600 m east
             (OUT_M + LANES_M + 0.5, "Make a U-turn"),
             (OUT_M + LANES_M + 400, "Turn right onto Elm St")]   # return leg, 600 m east
    return pts, turns


def test_on_an_out_and_back_the_turns_ahead_are_the_ones_for_this_leg(device, link):
    """The way out and the way back pass the same spot. At 500 m east on the
    way out the next turn is the outbound fork 100 m ahead, then the U-turn;
    after the U-turn, at 700 m east on the way back, it is the return leg's
    turn 100 m ahead — never the other leg's cue at the same place."""
    pts, turns = out_and_back()
    navigate(device, link, pts, turns)
    nav = hold(device, *offset(LAT0, LON0, LANES_M / 2, 300))
    nav = hold(device, *offset(LAT0, LON0, LANES_M / 2, 500))
    assert nav["next"] == "Keep left at the fork" and abs(nav["next_m"] - 100) < 8, nav
    nav = hold(device, *offset(LAT0, LON0, LANES_M / 2, 750))
    assert nav["next"] == "Make a U-turn" and abs(nav["next_m"] - 258) < 10, nav
    for x in (900, 990):
        hold(device, *offset(LAT0, LON0, LANES_M / 2, x))
    for x in (950, 850, 700):                                   # back west, south side
        nav = hold(device, *offset(LAT0, LON0, -LANES_M / 2, x))
    assert nav["next"] == "Turn right onto Elm St" and abs(nav["next_m"] - 100) < 10, nav
    assert abs(nav["remaining_km"] - 0.7) < 0.02, nav


@pytest.mark.xfail(strict=True, reason=(
    "BUG: routes.cpp updateProgress searches 2 km ahead of the rider but only 300 m back. On an "
    "out-and-back shorter than that, one fix nearer the return carriageway (a 6 m error across an "
    "8 m road) matches the return leg 1.2 km further along; the next fix's window then starts "
    "900 m past the rider, the way out is no longer in it, and the match stays on the return leg "
    "(it is only metres away, so the 60 m re-acquire never fires). The banner skips the U-turn and "
    "announces the way back's turns for the rest of the way out; remaining km drops by 1.2 km."))
def test_one_fix_across_the_road_on_an_out_and_back_does_not_skip_the_u_turn(device, link):
    """The same out-and-back, ridden out on the north side. One fix lands on
    the other carriageway — a few metres of GPS error — and the next ones are
    back where the rider is. The cue must come back to the turn the rider is
    riding towards, as it does on a straight route
    (test_a_wandering_fix_does_not_walk_through_the_turns)."""
    pts, turns = out_and_back()
    navigate(device, link, pts, turns)
    for x in (200, 400, 500):
        nav = hold(device, *offset(LAT0, LON0, LANES_M / 2, x))
    assert nav["next"] == "Keep left at the fork", nav
    hold(device, *offset(LAT0, LON0, -LANES_M / 2 - 1, 505))     # one fix across the road
    for x in (510, 520, 530):
        nav = hold(device, *offset(LAT0, LON0, LANES_M / 2, x))
    assert nav["next"] == "Keep left at the fork" and abs(nav["next_m"] - 70) < 10, nav
    assert abs(nav["remaining_km"] - (2 * OUT_M + LANES_M - 530) / 1000) < 0.03, nav


def test_rejoining_further_along_after_a_detour_picks_up_the_turns_ahead(device, link):
    """A rider leaves the route (a closed road) and rides a parallel street
    300 m away, rejoining 2 km further on. Once back on the route, the banner
    must show the turn ahead of the rejoin point with its real distance — not
    the turns that were skipped."""
    pts = line_track(LAT0, LON0, 4, step_m=20)
    turns = [(500, "Turn 1"), (1500, "Turn 2"), (2500, "Turn 3"), (3500, "Turn 4")]
    navigate(device, link, pts, turns, start=along(300))
    nav = hold(device, *along(300))
    assert nav["next"] == "Turn 1", nav
    for x in range(400, 2900, 400):                            # the detour, 300 m north
        hold(device, *along(x, 300), settle_s=0.6)
    for x in (2950, 2980, 3000, 3010, 3020, 3030):             # back on the route
        nav = hold(device, *along(x), settle_s=1.1)
    assert nav["next"] == "Turn 4" and abs(nav["next_m"] - 470) < 15, nav
    assert abs(nav["remaining_km"] - (4.0 - 3.03)) < 0.03, nav


def test_starting_navigation_midway_skips_the_turns_already_behind(device, link):
    """A rider who accepts a route standing 2.2 km along it (a shared start,
    a loop joined from home) gets the next turn ahead, not the first one."""
    pts = line_track(LAT0, LON0, 4, step_m=20)
    turns = [(500, "Turn 1"), (1500, "Turn 2"), (2500, "Turn 3"), (3500, "Turn 4")]
    navigate(device, link, pts, turns, start=along(2200))
    nav = hold(device, *along(2200))
    assert nav["next"] == "Turn 3" and abs(nav["next_m"] - 300) < 10, nav


def test_at_the_finish_the_arrival_cue_reads_zero_and_nothing_is_left(device, link):
    """The last cue is the arrival. Riding up to it the countdown reaches 0;
    standing at the end (and rolling a little past it) the banner keeps the
    arrival at 0 m and the remaining distance is 0 — it does not run on or go
    negative."""
    pts = line_track(LAT0, LON0, 1, step_m=20)
    navigate(device, link, pts, [(400, "Turn left"), (1000, "Arrive at destination")])
    hold(device, *along(800))
    nav = hold(device, *along(960))
    assert nav["next"] == "Arrive at destination" and abs(nav["next_m"] - 40) < 6, nav
    for x in (1000, 1015):
        nav = hold(device, *along(x))
        assert nav["next"] == "Arrive at destination" and nav["next_m"] < 1, nav
        assert nav["remaining_km"] < 0.002, nav


# -- the NAVIGATE prompt ------------------------------------------------------------------------

def prompt_route(device, link, at_m=0.0):
    """The rider stands `at_m` along a 2 km route east when it arrives."""
    pts = line_track(LAT0, LON0, 2, step_m=20)
    device.gps(*along(at_m), speed_kmh=0)
    device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    time.sleep(1.2)
    send_route(link, "t.gpx", pts, [along(800) + ("Turn left",)])
    device.wait_status(lambda s: s["nav"]["pending"], timeout=10)
    return pts


def test_riding_off_with_the_prompt_up_starts_navigation_by_itself(device, link):
    """A route arrives as the rider waits at its start and the prompt goes
    unanswered. Once they ride off along the route, navigation starts by
    itself (routes.cpp updateProgress grabs it): a rider can't be expected to
    stop for a tap."""
    prompt_route(device, link, 0)
    time.sleep(2)
    assert device.status()["nav"]["pending"]
    for x in (20, 60, 100, 140):
        hold(device, *along(x), settle_s=0.8)
    nav = device.wait_status(lambda s: s["nav"]["active"], timeout=5 * TIME_SCALE)["nav"]
    assert not nav["pending"] and nav["next"] == "Turn left", nav


@pytest.mark.xfail(strict=True, reason=(
    "BUG: routes.cpp updateProgress treats the first fix after a route loads as 'moving along' "
    "it (progIdx > prev, where prev is the 0 a load resets it to), so a rider standing still "
    "anywhere past the route's first point has the NAVIGATE prompt taken away within a second: "
    "navigation starts on its own and START / LATER are never offered."))
def test_a_rider_standing_on_the_route_keeps_the_prompt_until_they_answer(device, link):
    """The rider is stopped 100 m into the route (at home, the route starts
    down the street) when it arrives. Nobody has moved: the prompt must wait
    for START or LATER."""
    prompt_route(device, link, 100)
    for _ in range(3):
        hold(device, *along(100), settle_s=1.0)
    nav = device.status()["nav"]
    assert nav["pending"] and not nav["active"], nav


def test_back_on_the_prompt_keeps_the_route_on_the_map_without_navigating(device, link):
    """A route arrives while the rider is in the menu. HOME (back) on the
    prompt is "not now": the route stays loaded (drawn on the map, remaining
    distance live) but there is no turn banner — and riding along it
    afterwards must not start navigation behind the rider's back."""
    device.console("screen menu")
    device.wait_status(lambda s: s["screen"] == "other", timeout=5 * TIME_SCALE)
    prompt_route(device, link, 0)
    time.sleep(0.9)
    device.button("home")
    device.wait_status(lambda s: not s["nav"]["pending"], timeout=5 * TIME_SCALE)
    for x in (20, 60, 100, 140):
        nav = hold(device, *along(x), settle_s=0.8)
    assert nav["route"] and not nav["active"] and nav["next"] is None, nav
    assert abs(nav["remaining_km"] - 1.86) < 0.02, nav


def test_later_on_the_prompt_unloads_the_route_but_keeps_the_file(device, link):
    """Tapping anywhere but START is LATER: the route is unloaded (no line on
    the map), still on the card for NAVIGATE."""
    pts = line_track(LAT0, LON0, 2, step_m=20)
    device.gps(*along(0), speed_kmh=0)
    send_route(link, "later.gpx", pts, [along(800) + ("Turn left",)])
    device.wait_status(lambda s: s["nav"]["pending"], timeout=10)
    time.sleep(0.9)
    device.tap(270, 200)
    nav = device.wait_status(lambda s: not s["nav"]["pending"], timeout=5 * TIME_SCALE)["nav"]
    assert not nav["route"] and not nav["active"], nav
    assert os.listdir(os.path.join(device.sd, "routes")) == ["later.gpx"]


def test_a_tap_in_the_first_moment_of_the_prompt_does_not_answer_it(device, link):
    """A touch already under way when the route lands (the rider was using
    the panel) must not take the prompt's START or LATER: taps in its first
    700 ms are ignored."""
    pts = line_track(LAT0, LON0, 2, step_m=20)
    send_route(link, "t.gpx", pts, [along(800) + ("Turn left",)])
    device.wait_status(lambda s: s["nav"]["pending"], timeout=10)
    device.tap(*POWER_SHUTDOWN, ms=20)                          # START, at once
    nav = device.status()["nav"]
    assert nav["pending"] and not nav["active"], nav


def test_more_turns_than_the_device_holds_keeps_the_first_ones(device, link):
    """The device holds 128 cues (DeviceHello max_maneuvers); the apps cap
    what they send, but a longer list must still leave a working route: the
    first 128, in order, with the extra ones dropped."""
    pts = line_track(LAT0, LON0, 14, step_m=50)
    turns = [(100 * (i + 1), "Cue %d" % (i + 1)) for i in range(136)]
    device.gps(*along(0), speed_kmh=0)
    device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    assert upload(link, "many.gpx", gpx(pts)).code == pb.RESULT_OK
    for i in range(0, len(turns), 16):
        link.expect_result(maneuvers=pb.RouteManeuvers(items=[
            pb.Maneuver(lat_e7=int(round(at_along(pts, m)[0] * 1e7)), lon_e7=int(round(at_along(pts, m)[1] * 1e7)),
                        text=t) for m, t in turns[i:i + 16]]))
    link.expect_result(route_finish=pb.RouteFinish())
    accept_route(device)
    assert device.status()["nav"]["maneuvers"] == 128
    for x in range(1500, 12750, 1500):
        hold(device, *at_along(pts, x), settle_s=0.8)
    nav = hold(device, *at_along(pts, 12750))
    assert nav["next"] == "Cue 128" and abs(nav["next_m"] - 50) < 8, nav
    nav = hold(device, *at_along(pts, 12900))
    assert nav["next"] is None, nav


# -- the route on the map -----------------------------------------------------------------------

RIDER_X = 270
AHEAD = (215, 395)       # above the rider (north-up): below the turn banner, above "NO MAP HERE"
BEHIND = (505, 575)      # below it, down to the route's start 300 m (150 px) south


def column_runs(g, x, y0, y1):
    """Dark/light runs down the route's centre line, as (dark?, length)."""
    runs = []
    for y in range(y0, y1):
        dark = g.px(x, y) <= 0x40
        if runs and runs[-1][0] == dark:
            runs[-1][1] += 1
        else:
            runs.append([dark, 1])
    return runs


def route_north_on_the_map(device, link, tmp_path, step_m):
    """A 2 km route due north, the rider 300 m along it, the map at 2 m/px:
    the route runs straight up the panel's centre column from the rider."""
    pts = line_track(LAT0, LON0, 2, bearing_deg=0, step_m=step_m)
    rider = offset(LAT0, LON0, 300, 0)
    navigate(device, link, pts, [(1500, "Turn left")], start=rider)
    hold(device, *rider)
    device.console("screen map")
    device.wait_status(lambda s: s["screen"] == "map", timeout=5 * TIME_SCALE)
    time.sleep(2.5)
    return glass(device, tmp_path, "map_%d" % step_m)


def test_the_route_already_ridden_is_drawn_solid(device, link, tmp_path):
    """Behind the rider the route is one unbroken 14 px line."""
    g = route_north_on_the_map(device, link, tmp_path, 20)
    runs = column_runs(g, RIDER_X, *BEHIND)
    assert runs == [[True, BEHIND[1] - BEHIND[0]]], "behind the rider: %s" % runs[:12]


@pytest.mark.xfail(strict=True, reason=(
    "BUG: the route ahead is meant to be dashed (map_view.cpp: 'ridden part solid, ahead dashed', "
    "Style {14, 0, 26, 16}), but drawSegmentStyled restarts the dash at every polyline vertex, so "
    "a route whose points are closer than 26 px (52 m at the default 2 m/px; a 20 m-spaced GPX is "
    "10 px) is drawn entirely as dash with no gap, and where a gap does fall thickSegment's round "
    "caps (radius 7) close 14 of its 16 px. On the glass the route ahead looks solid, like the "
    "part already ridden."))
@pytest.mark.parametrize("step_m", [20, 200])
def test_the_route_ahead_is_drawn_dashed(device, link, tmp_path, step_m):
    """Ahead of the rider the route is dashed, so the part still to ride
    reads differently from the part behind: along its centre line there are
    clear gaps (at least 6 px of white, several of them), whether the GPX has
    a point every 20 m (a recorded track) or every 200 m (a router's line)."""
    g = route_north_on_the_map(device, link, tmp_path, step_m)
    runs = column_runs(g, RIDER_X, *AHEAD)
    assert sum(n for dark, n in runs if dark) > 0.4 * (AHEAD[1] - AHEAD[0]), "no route ahead: %s" % runs
    gaps = [n for dark, n in runs[1:-1] if not dark]
    assert len([n for n in gaps if n >= 6]) >= 3, "ahead of the rider: %s" % runs


# -- long routes ---------------------------------------------------------------------------------

@pytest.mark.xfail(strict=True, reason=(
    "BUG: routes.cpp GpxScanner thins a track over 4096 points by keeping every stride-th point "
    "(stride doubling at each halving) and never keeps the last one, so the route ends up to "
    "stride-1 points short of its finish (7 at 16k+ points): this 20,005-point GPX (a 200 km "
    "track at 10 m) ends 60 m before the destination, and 100 m out the device says 40 m are "
    "left. (The apps' importers keep the last point when they thin; the device doesn't. The "
    "chunk re-scan bug above shifts which points survive, so the shortfall varies 0-70 m.)"))
def test_a_very_long_track_from_the_card_still_ends_at_its_finish(device):
    """A 200 km track recorded at 10 m (20,000 points, ~0.9 MB) copied onto
    the card. The device keeps 4096 points at most and thins the rest; the
    route must still run to the file's last point: a rider 60 m from the
    finish has 100 m left, not 40."""
    pts = line_track(LAT0, LON0, 201, step_m=10)[:20005]
    with open(os.path.join(device.sd, "routes", "big.gpx"), "wb") as f:
        f.write(gpx(pts, "Big"))
    open_navigate(device)
    t0 = time.time()
    name, n, km = pick_route(device, 0)
    assert time.time() - t0 < 5 * TIME_SCALE, "loading a 0.9 MB route took %.1f s" % (time.time() - t0)
    assert name == "big.gpx" and 2048 <= n <= 4096, (name, n)
    near_end = at_along(pts, length_m(pts) - 100)
    for _ in range(6):                     # joined far from where it started: re-acquired
        nav = hold(device, *near_end, settle_s=0.8)
    assert abs(nav["remaining_km"] * 1000 - 100) < 12, "100 m from the finish, %.0f m left" % (
        nav["remaining_km"] * 1000)


def test_a_route_of_4096_points_is_followed_without_slowing_the_ride(device, link):
    """The biggest route the app sends (GpxImporter.MAX_POINTS = 4096, a
    100 km ride at 25 m): the device keeps every point, follows the rider
    along it on every fix and keeps drawing the map once a second while
    recording."""
    pts = line_track(LAT0, LON0, 102.4, bearing_deg=60, step_m=25)[:4096]
    data = gpx(pts, "Century")
    assert len(data) < 256 * 1024
    navigate(device, link, pts, [(50000, "Turn left at the halfway café"),
                                 (length_m(pts) - 1, "Arrive at destination")],
             start=at_along(pts, 49000))
    assert route_lines(device)[-1][1] == 4096
    device.console("screen map")
    device.button("boot")
    device.wait_status(lambda s: s["ride"]["recording"], timeout=10)
    seg = [at_along(pts, 49000 + 10 * i) for i in range(160)]
    device.gps_track(seg, speed_kmh=36)
    p0 = device.status()["paints"]
    time.sleep(10)
    st = device.status()
    assert st["paints"] - p0 >= 6, "%d paints in 10 s of riding on a 4096-point route" % (st["paints"] - p0)
    a = along_of(pts, st["ride"]["lat"], st["ride"]["lon"])
    assert st["nav"]["next"] == "Turn left at the halfway café"
    assert abs(st["nav"]["next_m"] - (50000 - a)) < 15, (st["nav"], a)


# -- navigating while recording ------------------------------------------------------------------

def test_a_ride_recorded_while_navigating_saves_and_the_navigation_carries_on(device, link):
    """Most routes are ridden while recording. Starting the ride, stopping it
    at a café and saving it must not end the navigation (the turns still
    count down from where the rider is), and the saved FIT is the ride."""
    from test_rides import SAVE_BUTTON, check_fit
    pts = line_track(LAT0, LON0, 3, step_m=20)
    navigate(device, link, pts, [(2500, "Turn right onto Main St")])
    device.button("boot")
    device.wait_status(lambda s: s["ride"]["recording"], timeout=10)
    device.set_speed(10)
    device.gps_track(pts[:76], speed_kmh=36)                    # 1.5 km
    device.wait_status(lambda s: s["gps"]["track_left"] == 0, timeout=60 * TIME_SCALE)
    device.set_speed(1)
    nav = hold(device, *pts[75])
    assert nav["next"] == "Turn right onto Main St" and abs(nav["next_m"] - 1000) < 15, nav
    device.button("boot")                                       # stop: the summary
    device.wait_status(lambda s: s["screen"] == "other", timeout=10)
    device.tap(*SAVE_BUTTON)
    st = device.wait_status(lambda s: not s["ride"]["recording"], timeout=20)
    assert st["nav"]["active"] and st["nav"]["next"] == "Turn right onto Main St", st["nav"]
    rides = [f for p in link.request(list=pb.ListRequest(kind=pb.LIST_RIDES), all_pages=True)
             for f in p.file_list.files]
    assert len(rides) == 1
    check_fit(link.download(pb.SOURCE_RIDE, rides[0].name))
    nav = hold(device, *at_along(pts, 2000))
    assert abs(nav["next_m"] - 500) < 15, nav
