"""The STOPWATCH page (src/stopwatch.h, ui_dashboard's page and the link's
StopwatchCommand / LiveFrame StopwatchState): the rider's own clock, its
three buttons and their rules, laps and their averages, and what the app's
copy of the page is told. The clock and laps are read from the status
JSON's "stopwatch" block and from the app's LIVE_TOPIC_STOPWATCH frames."""
import time

from otpsim import pb
from test_regressions_ui import glass, pair_sensor

TOGGLE = (24 + 102, 780 + 78)    # kStopwatchToggle centre (ui_render.cpp)
RESET = (240 + 66, 780 + 78)     # kStopwatchReset
LAP = (384 + 66, 780 + 78)       # kStopwatchLap


def stopwatch_page(device, link):
    r = link.request(set_dash=pb.DashConfig(text="speed hero\npage stopwatch\n"))
    assert r.WhichOneof("body") == "dash"
    link.tell(show_page=pb.ShowPage(page=1))
    device.wait_status(lambda s: s["page"] == 1 and s["screen"] == "dash", timeout=5)
    time.sleep(1.2)


def tap(device, xy):
    device.tap(*xy)
    time.sleep(0.45)   # one tap per release, 350 ms debounce


def sw(device):
    return device.status()["stopwatch"]


def op(link, o):
    link.tell(stopwatch=pb.StopwatchCommand(op=o))


def frame(link, predicate=lambda w: True, timeout=5):
    return link.wait_push("live", lambda f: f.HasField("stopwatch") and predicate(f.stopwatch),
                          timeout=timeout).stopwatch


def test_the_page_buttons_and_their_rules(device, link):
    """START runs it; LAP only while running; RESET only once paused — a
    stray tap mid-effort never throws the time away; a paused clock holds."""
    stopwatch_page(device, link)
    tap(device, LAP)
    tap(device, RESET)
    assert sw(device) == {"running": False, "elapsed_ms": 0, "laps": 0, "lap_ms": 0}
    tap(device, TOGGLE)
    assert sw(device)["running"]
    time.sleep(1)
    tap(device, RESET)                                  # running: does nothing
    s = sw(device)
    assert s["running"] and s["elapsed_ms"] >= 1000
    tap(device, LAP)
    s = device.wait_status(lambda s: s["stopwatch"]["laps"] == 1, timeout=2)["stopwatch"]
    assert s["lap_ms"] < s["elapsed_ms"]
    tap(device, TOGGLE)                                 # pause
    held = sw(device)
    assert not held["running"]
    tap(device, LAP)                                    # paused: no lap
    time.sleep(1)
    s = sw(device)
    assert s["laps"] == 1 and s["elapsed_ms"] == held["elapsed_ms"]
    tap(device, RESET)
    assert sw(device) == {"running": False, "elapsed_ms": 0, "laps": 0, "lap_ms": 0}


def test_it_keeps_time_off_its_page(device, link):
    """It is time stamps, not a per-frame counter: on the map, in the menu,
    it runs on."""
    stopwatch_page(device, link)
    tap(device, TOGGLE)
    link.tell(show_page=pb.ShowPage(page=0))
    device.wait_status(lambda s: s["page"] == 0, timeout=3)
    device.tap(270, 30)                                 # the menu
    device.set_speed(10)
    time.sleep(1.5)
    device.set_speed(1)
    s = sw(device)
    assert s["running"] and s["elapsed_ms"] >= 14000


def test_the_app_drives_it_and_sees_every_lap(device, link):
    """The app's buttons are queued to the page's own handlers, with the
    same rules, and its copy of the lap table is newest first with each
    lap's own split."""
    link.expect_result(live_subscribe=pb.LiveSubscribe(topics=[pb.LIVE_TOPIC_STOPWATCH]))
    op(link, pb.STOPWATCH_OP_LAP)                       # not running: ignored
    op(link, pb.STOPWATCH_OP_TOGGLE)
    frame(link, lambda w: w.running)
    marks = []
    for _ in range(3):
        time.sleep(0.4)
        op(link, pb.STOPWATCH_OP_LAP)
        n = len(marks) + 1
        device.wait_status(lambda s: s["stopwatch"]["laps"] == n, timeout=3)
        marks.append(n)
    op(link, pb.STOPWATCH_OP_RESET)                     # running: ignored
    w = frame(link, lambda w: w.laps == 3)
    assert w.running
    assert [l.number for l in w.recent] == [3, 2, 1]
    assert all(300 <= l.split_ms <= 1500 for l in w.recent), [l.split_ms for l in w.recent]
    assert w.last_split_ms == w.recent[0].split_ms
    assert not w.shows_power and not any(l.HasField("power_w") for l in w.recent)
    op(link, pb.STOPWATCH_OP_TOGGLE)
    op(link, pb.STOPWATCH_OP_RESET)
    w = frame(link, lambda w: not w.running and w.elapsed_ms == 0)
    assert w.laps == 0 and len(w.recent) == 0 and not w.HasField("last_split_ms")


def test_a_lap_averages_the_power_meter(device, link):
    """With a meter reporting, each lap carries its average power and
    cadence, and the page grows those columns."""
    pair_sensor(device, link, "power", watts=240, cadence=92)
    device.wait_status(lambda s: s["ride"]["power"] == 240, timeout=30)
    link.expect_result(live_subscribe=pb.LiveSubscribe(topics=[pb.LIVE_TOPIC_STOPWATCH]))
    op(link, pb.STOPWATCH_OP_TOGGLE)
    time.sleep(2)
    op(link, pb.STOPWATCH_OP_LAP)
    w = frame(link, lambda w: w.laps == 1)
    assert w.shows_power and w.shows_cadence
    assert w.recent[0].power_w == 240
    assert abs(w.recent[0].cadence_rpm - 92) <= 2      # from crank revolutions: a rounding either way
    assert not w.recent[0].HasField("speed_dkmh"), "no speed source: no speed average"


def test_the_lap_table_is_drawn_once_there_are_laps(device, link, tmp_path):
    """Before the first LAP the page is the time alone; after, the list of
    laps appears under it."""
    stopwatch_page(device, link)
    tap(device, TOGGLE)
    before = glass(device, tmp_path, "nolaps")
    for _ in range(3):
        tap(device, LAP)
    time.sleep(1.2)
    after = glass(device, tmp_path, "laps")
    numbers = (20, 390, 50, 540)                        # the lap numbers' column, left of the clock
    assert before.ink(*numbers) < 0.01
    assert after.ink(*numbers) > 0.1, "no lap rows drawn"


def test_the_app_sees_the_last_seven_laps(device, link):
    """The page's table holds seven rows; the app is sent the same seven."""
    link.expect_result(live_subscribe=pb.LiveSubscribe(topics=[pb.LIVE_TOPIC_STOPWATCH]))
    op(link, pb.STOPWATCH_OP_TOGGLE)
    for n in range(1, 10):
        op(link, pb.STOPWATCH_OP_LAP)
        device.wait_status(lambda s: s["stopwatch"]["laps"] == n, timeout=3)
    w = frame(link, lambda w: w.laps == 9)
    assert [l.number for l in w.recent] == [9, 8, 7, 6, 5, 4, 3]


def test_the_hundredth_lap_is_refused_not_lap_one_dropped(device, link):
    """99 laps fill the table (STOPWATCH_MAX_LAPS); LAP then does nothing."""
    op(link, pb.STOPWATCH_OP_TOGGLE)
    for n in range(1, 100):
        op(link, pb.STOPWATCH_OP_LAP)
        device.wait_status(lambda s: s["stopwatch"]["laps"] == n, timeout=3)
    op(link, pb.STOPWATCH_OP_LAP)
    time.sleep(0.5)
    s = sw(device)
    assert s["laps"] == 99 and s["running"]
