"""Structured workouts (src/workout.cpp, src/workout_service.cpp, the link's
TARGET_WORKOUT / WorkoutCommand, the WORKOUT page): files from the app, the
targets a rider is asked to hold, the clock through pauses, skips and block
boundaries, and the page's own touch targets.

The workout clock runs on device time (millis()), so the tests speed the
device clock up and check the model's own consistency — which block, which
target, at which elapsed second — rather than wall-clock timing. The model is
read from the status JSON's "workout" block (sim/PROTOCOL.md), which is
workout_service::view(), the same call the page and the link's
WorkoutStatus are built from."""
import os
import time

import pytest

from conftest import TIME_SCALE
from otpsim import LinkClient, pb
from otpsim.link import LinkError

LAT, LON = 45.70, -121.52


# -- workout files ------------------------------------------------------------------------

def erg(points, desc=None, percent=False, header=""):
    """An ERG (watts) or MRC (percent of FTP) file from [(minutes, value)]."""
    out = "[COURSE HEADER]\nVERSION = 2\nUNITS = ENGLISH\n"
    if desc:
        out += "DESCRIPTION = %s\n" % desc
    out += header
    out += "MINUTES PERCENT\n" if percent else "MINUTES WATTS\n"
    out += "[END COURSE HEADER]\n[COURSE DATA]\n"
    out += "".join("%.2f\t%g\n" % p for p in points)
    out += "[END COURSE DATA]\n"
    return out.encode()


def blocks(spec):
    """Flat blocks [(minutes, value)] as both apps' builders write them: two
    points per block, the second repeating at the block's end (WorkoutBuilder
    Sheet.kt send(), WorkoutsView.swift)."""
    pts, t = [], 0.0
    for minutes, v in spec:
        pts.append((t, v))
        t += minutes
        pts.append((t, v))
    return pts


THREE = blocks([(1, 100), (1, 200)]) + [(2, 200), (3, 300)]   # 0-60 s 100 W, 60-120 s 200 W, 120-180 s ramp


def upload(link, name, data):
    return link.upload(pb.TARGET_WORKOUT, name, data, timeout=20)


def cmd(link, op, **kw):
    r = link.request(workout=pb.WorkoutCommand(op=op, **kw))
    assert r.WhichOneof("body") == "workout_status", r
    return r.workout_status


def keep_moving(device):
    """A fix with speed: the workout's stillness auto-pause (motionTick) sees
    a rider on the move and leaves the clock alone."""
    device.gps(LAT, LON, speed_kmh=25)
    device.wait_status(lambda s: s["ride"]["gps_fix"] and s["ride"]["speed_kmh"] > 5, timeout=10)


def expected_target(e):
    """THREE's target at elapsed second e, as workoutTargetAt computes it."""
    if e < 60:
        return 100
    if e < 120:
        return 200
    if e >= 180:
        return 300
    return 200 + 100 * (e - 120) // 60


# -- uploads ------------------------------------------------------------------------------

def test_an_uploaded_workout_is_loaded_listed_and_ready(device, link):
    """The app's Send: the file is saved, loaded at once, and the device is
    ready at block 1 with its first target — not running until the rider
    says so."""
    done = upload(link, "Sweet_Spot.erg", erg(THREE, desc="Sweet Spot"))
    assert done.code == pb.RESULT_OK
    w = device.workout()
    assert w["loaded"] and not w["running"] and not w["paused"]
    # The page's title: the filename, extension dropped, upper case (the
    # Impact faces' subset).
    assert w["name"] == "SWEET_SPOT"
    assert (w["segments"], w["total_s"], w["target_w"], w["next_w"]) == (3, 180, 100, 200)
    st = link.wait_push("workout_status", lambda s: s.loaded, timeout=5)
    assert st.segment_count == 3 and st.total_s == 180 and st.target_w == 100
    names = [f.name for page in link.request(list=pb.ListRequest(kind=pb.LIST_WORKOUTS), all_pages=True)
             for f in page.file_list.files]
    assert "Sweet_Spot.erg" in names
    assert open(os.path.join(device.sd, "workouts", "Sweet_Spot.erg"), "rb").read() == erg(THREE, desc="Sweet Spot")


def test_a_percent_file_is_scaled_by_the_riders_ftp(device, link):
    """An .mrc is percent of FTP: the device turns it into watts with the
    FTP set on it, so 50 % of 200 W is a 100 W target."""
    link.request(set_settings=pb.Settings(ftp_w=200))
    upload(link, "half.mrc", erg(blocks([(1, 50), (1, 105)]), percent=True))
    w = device.workout()
    assert (w["target_w"], w["next_w"], w["ftp"]) == (100, 210, 200)


def test_an_ftp_line_in_the_file_wins_over_the_setting(device, link):
    """A file exported for a rider knows their FTP: `FTP = 300` in its
    header scales it, whatever the device's setting says."""
    link.request(set_settings=pb.Settings(ftp_w=200))
    upload(link, "coach.mrc", erg(blocks([(1, 50)]), percent=True, header="FTP = 300\n"))
    assert device.workout()["target_w"] == 150


@pytest.mark.parametrize("name,data,code", [
    ("empty.erg", b"", pb.RESULT_BAD_REQUEST),
    ("huge.erg", b"0 100\n" * 3000, pb.RESULT_TOO_LARGE),          # 18 KB: past workout_max_bytes
    ("../escape.erg", erg(THREE), pb.RESULT_BAD_REQUEST),
    (".hidden.erg", erg(THREE), pb.RESULT_BAD_REQUEST),
])
def test_uploads_the_device_cannot_take_are_refused_up_front(device, link, name, data, code):
    """Refused at UploadBegin, before a byte is sent: nothing reaches the card."""
    assert link.device_hello.workout_max_bytes == 16384
    with pytest.raises(LinkError) as e:
        upload(link, name, data)
    assert e.value.code == pb.ResultCode.values_by_number[code].name
    assert not os.path.exists(os.path.join(device.sd, "workouts", name.lstrip("./")))


@pytest.mark.parametrize("what,data", [
    ("no course data", b"[COURSE HEADER]\nMINUTES WATTS\n[END COURSE HEADER]\n"),
    ("a single point", erg([(0, 200)])),
    ("time never advances", erg([(0, 100), (0, 200), (0, 300)])),
    ("not a workout at all", b"\x89PNG\r\n\x1a\n" + bytes(range(256)) * 4),
])
def test_a_malformed_workout_is_refused_and_drops_the_old_one(device, link, what, data):
    """A file that parses to nothing is a PARSE_ERROR, and the workout that
    was loaded is dropped with it (workout_service::load: half a workout is
    worse than none) — the page must not keep asking for the old targets as
    if the new file had landed."""
    upload(link, "good.erg", erg(THREE))
    assert device.workout()["loaded"]
    with pytest.raises(LinkError) as e:
        upload(link, "bad.erg", data)
    assert e.value.code == "RESULT_PARSE_ERROR", what
    assert not device.workout()["loaded"]
    st = link.wait_push("workout_status", lambda s: not s.loaded, timeout=5)
    assert st.segment_count == 0


def test_the_longest_workout_the_parser_holds_loads_whole(device, link):
    """64 segments is the limit (WORKOUT_MAX_SEGS); a ramp-to-ramp file of
    exactly 64 loads every one of them."""
    pts = [(i * 0.5, 100 + (i % 7) * 20) for i in range(65)]
    upload(link, "max.erg", erg(pts))
    w = device.workout()
    assert (w["segments"], w["total_s"]) == (64, 64 * 30)


@pytest.mark.xfail(strict=True, reason=(
    "BUG: workoutParse (src/workout.cpp:23 pt[WORKOUT_MAX_SEGS + 1]) keeps only the first 65 POINTS, but "
    "both apps' builders write two points per flat block, so any workout of more than 32 blocks is silently "
    "cut short — a 40 x 30 s microburst set built in the app loads as 32 blocks / 16 minutes and the device "
    "reports it as complete half-way through. workout.h claims a 30/30 set for half an hour fits."))
def test_an_app_built_microburst_set_loads_every_block(device, link):
    spec = [(0.5, 400 if i % 2 == 0 else 150) for i in range(40)]
    upload(link, "microbursts.mrc", erg(blocks([(m, w * 100 // 250) for m, w in spec]), percent=True))
    w = device.workout()
    assert (w["segments"], w["total_s"]) == (40, 40 * 30)


@pytest.mark.xfail(strict=True, reason=(
    "BUG: workoutParse (src/workout.cpp:43) takes ANY header line containing 'PERCENT' as the MINUTES PERCENT "
    "column line, so an ERG (watts) file whose DESCRIPTION mentions percent — TrainerRoad's descriptions "
    "routinely say 'at 88-94 percent of FTP' — is read as percent of FTP: a 250 W target becomes 500 W at "
    "FTP 200."))
def test_percent_in_an_erg_description_does_not_make_it_a_percent_file(device, link):
    link.request(set_settings=pb.Settings(ftp_w=200))
    upload(link, "sst.erg", erg(blocks([(10, 250)]), desc="Sweet spot at 88-94 percent of FTP"))
    assert device.workout()["target_w"] == 250


def test_a_long_name_keeps_its_title_and_is_reloadable(device, link):
    """The page's title is the file name, cut to fit (40 chars): a 46-char
    name still loads, and LOAD by name brings it back after an unload."""
    name = "threshold_over_unders_with_short_sprints_x.erg"   # 46 chars
    upload(link, name, erg(THREE))
    w = device.workout()
    assert w["loaded"] and w["name"] == name.upper()[:39]
    cmd(link, pb.WORKOUT_OP_UNLOAD)
    r = link.request(workout=pb.WorkoutCommand(op=pb.WORKOUT_OP_LOAD, name=name))
    assert r.result.code == pb.RESULT_OK
    assert device.workout()["loaded"]


@pytest.mark.xfail(strict=True, reason=(
    "BUG: the device-side picker skips any file whose name is 48 chars or longer (workout_service.cpp scan(): "
    "strlen(base) >= sizeof(WorkoutFileInfo::file)), but the link accepts upload names up to 63 chars and the "
    "apps' builders name the file after the workout with no limit — a long-named workout loads once on Send, "
    "and after an unload it is missing from the WORKOUT page's list (LIST_WORKOUTS still shows it)."))
def test_a_workout_with_a_long_name_is_offered_by_the_picker(device, link):
    name = "over_unders_with_thirty_second_sprints_and_long_recovery.mrc"   # 60 chars
    upload(link, name, erg(blocks([(1, 60)]), percent=True))
    cmd(link, pb.WORKOUT_OP_UNLOAD)
    _show_workout_page(device, link)   # the picker rescans as it appears
    st = device.wait_status(lambda s: s["workout"]["picker"]["state"] == "ready", timeout=10)
    assert [r["file"] for r in st["workout"]["picker"]["rows"]] == [name]


def test_a_missing_file_does_not_load(device, link):
    """LOAD names a file that isn't on the card: refused, nothing loaded."""
    upload(link, "here.erg", erg(THREE))
    r = link.request(workout=pb.WorkoutCommand(op=pb.WORKOUT_OP_LOAD, name="gone.erg"))
    assert r.WhichOneof("body") == "result" and r.result.code != pb.RESULT_OK
    assert "not found" in r.result.detail
    assert not device.workout()["loaded"]
    r = link.request(workout=pb.WorkoutCommand(op=pb.WORKOUT_OP_LOAD, name="../etc.erg"))
    assert r.result.code == pb.RESULT_BAD_REQUEST


def test_a_deleted_workout_leaves_the_list(device, link):
    upload(link, "a.erg", erg(THREE))
    upload(link, "b.erg", erg(THREE))
    link.expect_result(delete_file=pb.DeleteRequest(kind=pb.LIST_WORKOUTS, name="a.erg"))
    names = [f.name for page in link.request(list=pb.ListRequest(kind=pb.LIST_WORKOUTS), all_pages=True)
             for f in page.file_list.files]
    assert names == ["b.erg"]


# -- the clock ------------------------------------------------------------------------------

def test_blocks_change_at_their_boundaries_and_ramps_climb(device, link):
    """Ridden through at 20x: every sample of the model has the block and the
    target the file says for its elapsed second — steps at 60 s and 120 s, a
    linear ramp after, and the last target held once it is done."""
    keep_moving(device)
    upload(link, "three.erg", erg(THREE))
    st = cmd(link, pb.WORKOUT_OP_START)
    assert st.running and st.segment == 0
    device.set_speed(20)
    seen, ramp = set(), set()
    deadline = time.time() + 30 * TIME_SCALE
    while time.time() < deadline:
        w = device.workout()
        e = w["elapsed_s"]
        if w["done"]:
            break
        seg = 0 if e < 60 else 1 if e < 120 else 2
        assert w["segment"] == seg, w
        assert w["target_w"] == expected_target(e), w
        assert w["segment_left_s"] == (seg + 1) * 60 - e, w
        seen.add(seg)
        if seg == 2:
            ramp.add(w["target_w"])
        time.sleep(0.05)
    device.set_speed(1)
    w = device.workout()
    assert w["done"] and w["elapsed_s"] == 180 and w["target_w"] == 300 and w["segment"] == 2
    assert seen == {0, 1, 2}
    assert len(ramp) >= 3, "the ramp should be seen climbing, not jumping: %s" % sorted(ramp)


def test_the_app_sees_the_clock_once_a_second_while_it_runs(device, link):
    keep_moving(device)
    upload(link, "three.erg", erg(THREE))
    cmd(link, pb.WORKOUT_OP_START)
    link.drain_pushes()
    got = []
    t0 = time.time()
    while time.time() - t0 < 4:
        try:
            got.append(link.wait_push("workout_status", timeout=2))
        except TimeoutError:
            break
    assert len(got) >= 3, "WorkoutStatus is pushed 1 Hz while running"
    assert all(s.running for s in got)
    assert [s.elapsed_s for s in got] == sorted(s.elapsed_s for s in got)


def test_pause_holds_the_clock_and_resume_carries_on_from_it(device, link):
    keep_moving(device)
    upload(link, "three.erg", erg(THREE))
    cmd(link, pb.WORKOUT_OP_START)
    device.set_speed(10)
    device.wait_status(lambda s: s["workout"]["elapsed_s"] >= 20, timeout=10)
    held = cmd(link, pb.WORKOUT_OP_PAUSE)
    assert held.paused and not held.running
    time.sleep(1.5)   # 15 s of device time
    w = device.workout()
    assert w["paused"] and w["elapsed_s"] == held.elapsed_s
    st = cmd(link, pb.WORKOUT_OP_RESUME)
    assert st.running and held.elapsed_s <= st.elapsed_s <= held.elapsed_s + 1
    device.wait_status(lambda s: s["workout"]["elapsed_s"] >= held.elapsed_s + 10, timeout=10)
    device.set_speed(1)


def test_skip_back_and_jump_move_between_blocks(device, link):
    keep_moving(device)
    upload(link, "three.erg", erg(THREE))
    cmd(link, pb.WORKOUT_OP_START)
    st = cmd(link, pb.WORKOUT_OP_SKIP)            # the warmup is over, I'm warm
    assert (st.segment, st.elapsed_s, st.target_w) == (1, 60, 200) and st.running
    st = cmd(link, pb.WORKOUT_OP_JUMP, segment=2)
    assert (st.segment, st.elapsed_s, st.target_w) == (2, 120, 200) and st.running
    st = cmd(link, pb.WORKOUT_OP_JUMP, segment=0)
    assert (st.segment, st.elapsed_s) == (0, 0)
    st = cmd(link, pb.WORKOUT_OP_JUMP, segment=99)   # past the end: the last block
    assert (st.segment, st.elapsed_s) == (2, 120)
    st = cmd(link, pb.WORKOUT_OP_SKIP)            # skipping the last block finishes the session
    assert st.done and st.elapsed_s == 180
    # The console's `workout back` (prevInterval): deep in a block it restarts
    # it; within its first 3 s it goes to the one before.
    cmd(link, pb.WORKOUT_OP_JUMP, segment=1)
    device.console("workout back")
    assert device.workout()["segment"] == 0


def test_stop_keeps_the_workout_and_unload_puts_it_away(device, link):
    keep_moving(device)
    upload(link, "three.erg", erg(THREE))
    cmd(link, pb.WORKOUT_OP_START)
    cmd(link, pb.WORKOUT_OP_SKIP)
    st = cmd(link, pb.WORKOUT_OP_STOP)
    assert st.loaded and not st.running and not st.paused and st.elapsed_s == 0 and st.segment == 0
    st = cmd(link, pb.WORKOUT_OP_START)            # start again: from the top
    assert st.running and st.elapsed_s == 0
    st = cmd(link, pb.WORKOUT_OP_UNLOAD)
    assert not st.loaded and not st.running and st.ftp_w == device.status()["settings"]["ftp"]
    st = cmd(link, pb.WORKOUT_OP_START)            # nothing to start
    assert not st.running and not st.loaded


def test_pause_each_block_holds_exactly_at_the_boundary(device, link):
    """The 'pause after every block' switch: the clock stops AT the next
    block's first second, so resuming rides all of it; skipping into a block
    is a deliberate start and is not held."""
    keep_moving(device)
    st = cmd(link, pb.WORKOUT_OP_PAUSE_EACH, pause_each=True)
    assert st.pause_each
    upload(link, "three.erg", erg(THREE))
    cmd(link, pb.WORKOUT_OP_START)
    device.set_speed(10)
    w = device.wait_status(lambda s: s["workout"]["paused"], timeout=15 * TIME_SCALE)["workout"]
    assert (w["segment"], w["elapsed_s"], w["target_w"]) == (1, 60, 200)
    time.sleep(1)
    assert device.workout()["elapsed_s"] == 60, "held, not creeping on"
    st = cmd(link, pb.WORKOUT_OP_RESUME)
    assert st.running
    device.wait_status(lambda s: s["workout"]["elapsed_s"] >= 70, timeout=10)
    st = cmd(link, pb.WORKOUT_OP_JUMP, segment=2)   # tapped a block: ride it
    time.sleep(1.5)
    assert device.workout()["running"]
    device.set_speed(1)
    # The switch is a setting: it survives a reboot.
    device.reboot()
    assert device.workout()["pause_each"]


def test_a_still_rider_pauses_the_clock_and_moving_again_resumes_it(device, link):
    """No power and no movement for 5 s pauses a running workout (stillness
    auto-pause); movement brings it back by itself. An explicit pause is the
    rider's and stays put when they start moving."""
    upload(link, "three.erg", erg(THREE))
    cmd(link, pb.WORKOUT_OP_START)
    w = device.wait_status(lambda s: s["workout"]["paused"], timeout=12 * TIME_SCALE)["workout"]
    assert 4 <= w["elapsed_s"] <= 7
    device.wait_log(r"workout: auto-paused", since=0, timeout=2)
    keep_moving(device)
    device.wait_status(lambda s: s["workout"]["running"], timeout=5 * TIME_SCALE)
    cmd(link, pb.WORKOUT_OP_PAUSE)
    time.sleep(2.5)
    assert device.workout()["paused"], "an explicit pause is not auto-resumed"


def test_power_alone_keeps_a_trainer_workout_running(device, link):
    """On a trainer nothing moves: a power meter reporting watts is what
    keeps the clock going."""
    addr = device.sensor("power", watts=210, cadence=88)
    link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_SCAN_START))
    link.wait_push("sensor_list", lambda l: any(s.address == addr for s in l.sensors), timeout=10)
    link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_PAIR, address=addr))
    link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_SCAN_STOP))
    device.wait_status(lambda s: s["ride"]["power"] == 210, timeout=20)
    upload(link, "three.erg", erg(THREE))
    cmd(link, pb.WORKOUT_OP_START)
    time.sleep(8)
    w = device.workout()
    assert w["running"] and w["elapsed_s"] >= 6


def test_a_workout_does_not_survive_a_reboot_but_its_file_does(device, phone, link):
    """The session is RAM only (workout_service.h): after a reset the page is
    back to the picker, and the file is still there to load again."""
    keep_moving(device)
    upload(link, "three.erg", erg(THREE))
    cmd(link, pb.WORKOUT_OP_START)
    device.reboot()
    assert not device.workout()["loaded"]
    phone.wait_disconnected(10)
    phone.connect()
    assert phone.wait_security(10)["encrypted"]
    link2 = LinkClient(phone).hello()
    st = link2.wait_push("workout_status", timeout=5)
    assert not st.loaded
    r = link2.request(workout=pb.WorkoutCommand(op=pb.WORKOUT_OP_LOAD, name="three.erg"))
    assert r.result.code == pb.RESULT_OK
    assert device.workout()["segments"] == 3


def test_loading_another_workout_stops_the_one_running(device, link):
    keep_moving(device)
    upload(link, "three.erg", erg(THREE))
    cmd(link, pb.WORKOUT_OP_START)
    upload(link, "short.erg", erg(blocks([(2, 150)])))
    w = device.workout()
    assert w["loaded"] and not w["running"] and not w["paused"] and w["total_s"] == 120


# -- the WORKOUT page ------------------------------------------------------------------------

ROW_TOP, ROW_H = 143, 88                       # kWorkoutRowTop / kWorkoutRowH (ui_render.cpp)
REDO = (24 + 120, 754 + 52)                    # kWorkoutRedo centre: back to the picker / previous block
NEXT = (276 + 120, 754 + 52)                   # kWorkoutStartNext
PAUSE = (24 + 120, 868 + 40)                   # kWorkoutPause: START / PAUSE BLOCK
ALL = (276 + 120, 868 + 40)                    # kWorkoutAll: ALL BLOCKS
CLOSE = (403 + 56, 866 + 40)                   # the list's pager action (kPagerAction): CLOSE


def _show_workout_page(device, link):
    """A layout with a WORKOUT page (slot 1), shown on the panel."""
    r = link.request(set_dash=pb.DashConfig(text="speed hero\npage workout\n"))
    assert r.WhichOneof("body") == "dash", r
    link.tell(show_page=pb.ShowPage(page=1))
    device.wait_status(lambda s: s["screen"] == "dash" and s["page"] == 1, timeout=5)


def tap(device, xy):
    device.tap(*xy)
    time.sleep(0.45)   # one tap per release, 350 ms debounce (ui_dashboard.cpp)


def test_the_picker_lists_the_card_and_a_tap_loads_one(device, link):
    """The WORKOUT page with nothing loaded lists /workouts by title; a row
    tap loads that file, and the page flips to READY."""
    upload(link, "b_vo2.erg", erg(THREE, desc="VO2 Max 5x3"))
    upload(link, "a_end.mrc", erg(blocks([(30, 65)]), percent=True, desc="Endurance"))
    cmd(link, pb.WORKOUT_OP_UNLOAD)
    _show_workout_page(device, link)
    st = device.wait_status(lambda s: s["workout"]["picker"]["state"] == "ready"
                            and s["workout"]["picker"]["total"] == 2, timeout=10)
    rows = st["workout"]["picker"]["rows"]
    assert [r["title"] for r in rows] == ["Endurance", "VO2 Max 5x3"]
    assert rows[1]["ok"] and rows[1]["total_s"] == 180 and rows[1]["segments"] == 3
    time.sleep(1.2)                                   # the picker is on the glass
    tap(device, (270, ROW_TOP + 1 * ROW_H + ROW_H // 2))
    w = device.wait_status(lambda s: s["workout"]["loaded"], timeout=5)["workout"]
    assert w["name"] == "B_VO2" and not w["running"]
    link.wait_push("workout_status", lambda s: s.loaded and s.name == "B_VO2", timeout=5)


def test_an_unreadable_file_is_listed_but_cannot_be_loaded(device, link):
    os.makedirs(os.path.join(device.sd, "workouts"), exist_ok=True)
    with open(os.path.join(device.sd, "workouts", "broken.erg"), "wb") as f:
        f.write(b"[COURSE DATA]\nnot numbers\n")
    _show_workout_page(device, link)
    st = device.wait_status(lambda s: s["workout"]["picker"]["state"] == "ready"
                            and s["workout"]["picker"]["total"] == 1, timeout=10)
    assert not st["workout"]["picker"]["rows"][0]["ok"]
    time.sleep(1.2)
    tap(device, (270, ROW_TOP + ROW_H // 2))
    time.sleep(1)
    assert not device.workout()["loaded"]


def test_the_page_buttons_start_pause_and_pick_blocks(device, link):
    """READY → START BLOCK runs it; PAUSE BLOCK holds it; ALL BLOCKS lists
    the session and a row starts that block; the next-block strip starts the
    next one and the previous-block strip goes back one."""
    keep_moving(device)
    upload(link, "three.erg", erg(THREE))
    _show_workout_page(device, link)
    time.sleep(1.2)
    tap(device, PAUSE)
    device.wait_status(lambda s: s["workout"]["running"], timeout=3)
    tap(device, PAUSE)
    device.wait_status(lambda s: s["workout"]["paused"], timeout=3)
    tap(device, ALL)
    time.sleep(0.8)                                   # the list is drawn
    tap(device, (270, ROW_TOP + 2 * ROW_H + ROW_H // 2))
    w = device.wait_status(lambda s: s["workout"]["segment"] == 2, timeout=3)["workout"]
    assert w["running"] and w["elapsed_s"] in (120, 121)
    time.sleep(0.8)
    tap(device, REDO)
    w = device.wait_status(lambda s: s["workout"]["segment"] == 1, timeout=3)["workout"]
    assert w["elapsed_s"] in (60, 61)
    tap(device, NEXT)
    device.wait_status(lambda s: s["workout"]["segment"] == 2, timeout=3)


def test_back_from_ready_returns_to_the_picker(device, link):
    """Loaded but not started, the left strip reads '< BACK / ALL WORKOUTS':
    it unloads, and the app is told."""
    upload(link, "three.erg", erg(THREE))
    _show_workout_page(device, link)
    time.sleep(1.2)
    link.drain_pushes()
    tap(device, REDO)
    device.wait_status(lambda s: not s["workout"]["loaded"], timeout=3)
    link.wait_push("workout_status", lambda s: not s.loaded, timeout=5)


@pytest.mark.xfail(strict=True, reason=(
    "BUG: on the last block the next-block strip reads 'NEXT > / SESSION COMPLETE' (ui_render.cpp "
    "ui_render_workout), but a tap on it runs jumpToSeg(segIdx + 1) (ui_dashboard.cpp handleTap, WORKOUT page), "
    "which clamps to the last block and restarts it from its first second — the rider asking to finish gets "
    "the cool-down again."))
def test_session_complete_on_the_last_block_finishes_the_session(device, link):
    keep_moving(device)
    upload(link, "three.erg", erg(THREE))
    _show_workout_page(device, link)
    cmd(link, pb.WORKOUT_OP_JUMP, segment=2)
    device.set_speed(10)
    device.wait_status(lambda s: s["workout"]["elapsed_s"] >= 150, timeout=10)
    device.set_speed(1)
    time.sleep(1.2)
    tap(device, NEXT)
    time.sleep(0.5)
    w = device.workout()
    assert w["done"] or w["elapsed_s"] >= 150, w


def _white_run(g, y, x0=468, x1=520):
    """White pixels across row y of the correction arrow's box (drawn white
    on the page's black block)."""
    return sum(1 for x in range(x0, x1) if g.px(x, y) >= 0xC0)


def test_the_arrow_says_which_way_to_go(device, link, tmp_path):
    """The dark block's correction: below the target band an up arrow (GO
    HARDER), above it a down arrow (EASE OFF), inside ±8 W the two bars of
    IN RANGE — the rider's power against the block's target."""
    from test_regressions_ui import glass, pair_sensor
    addr = pair_sensor(device, link, "power", watts=150, cadence=90)
    device.wait_status(lambda s: s["ride"]["power"] == 150, timeout=30)
    upload(link, "steady.erg", erg(blocks([(30, 200)])))
    _show_workout_page(device, link)
    cmd(link, pb.WORKOUT_OP_START)

    def arrow(name):
        time.sleep(4.5)                                   # 3 s power settles, a frame is drawn
        g = glass(device, tmp_path, name)
        return _white_run(g, 290), _white_run(g, 300), _white_run(g, 312)

    top, mid, bottom = arrow("harder")                   # 150 W against 200 W
    assert 0 < top < mid < bottom and bottom > 30, (top, mid, bottom)
    device.sensor("power", addr=addr, watts=260, cadence=90)
    top, mid, bottom = arrow("easier")
    assert top > mid > bottom > 0 and top > 30, (top, mid, bottom)
    device.sensor("power", addr=addr, watts=204, cadence=90)
    top, mid, bottom = arrow("in_range")
    assert mid == 0 and top >= 35 and bottom >= 35, (top, mid, bottom)
