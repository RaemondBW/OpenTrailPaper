"""The device against realistic SD cards (otpsim/cards.py): an empty card, a
season's worth (`typical`), years of it (`heavy`), and a card with only a few
MB left (`nearly_full`) — booting, listing to the app, paging on the panel,
recording — and against a card that fills up: mid-ride, and under uploads.

Card images are built once per session and cloned into each device. The
`heavy` profile and the ride that fills the card are marked slow (--slow).

Where the firmware misbehaves the test is a strict xfail naming the bug: it
documents today's behaviour, and turns into a failure the day it's fixed."""
import hashlib
import os
import random
import re
import shutil
import struct
import tempfile
import time

import pytest

from conftest import ARTIFACTS, paired_phone
from otpsim import LinkClient, LinkError, SimDevice, cards, pb
from test_regressions_rides import HISTORY_ROW, fit_crc, read_png
from test_rides import SAVE_BUTTON, check_fit, line_track, record_ride

FAST = ["--gps-baud", "115200"]
SLOW = pytest.mark.slow
PROFILES = ["empty", "typical", "nearly_full", pytest.param("heavy", marks=SLOW)]

# Boot timing runs with every card operation costing 2 ms of device time, so
# card work that grows with the card shows up (the sim's card is otherwise
# free). Measured (2026-10-26, M-series Mac), power-on to a painted dashboard:
# empty 1.2 s, typical 1.9 s, nearly_full 1.9 s, heavy 4.5-5.3 s — of which
# ~3.4 s is ride recovery walking all 1000 rides with openNextFile (an open per
# entry) before the dashboard's first paint. Bounds have ~2x headroom.
CARD_OP_MS = 2
BOOT_BOUND_S = {"empty": 4, "typical": 5, "nearly_full": 5, "heavy": 10}

LIST_MAX = 512        # link.cpp: files per FileList walk
TILE_LIST_MAX = 4096  # ble_server.cpp lockTileList
HISTORY_MAX = 256     # ui_dashboard.cpp kRideListMax: the newest 256, by design
ROWS = 5              # kMenuRowCount
PAGER_DOWN = (150 + 56, 866 + 40)   # ui_render.h kPagerDown centre


# ---- fixtures ------------------------------------------------------------------------------------

@pytest.fixture(scope="session")
def card_image(tmp_path_factory):
    """card_image(profile) -> cards.Card, built on first use in the session."""
    root = tmp_path_factory.mktemp("cards")
    built = {}

    def get(profile):
        if profile not in built:
            built[profile] = cards.build(profile, str(root / profile))
        return built[profile]
    return get


@pytest.fixture
def boot_card(request, card_image):
    """boot_card(profile, extra_args=()) -> (SimDevice, Card): a device whose
    card is a fresh clone of the profile's image."""
    devs = []

    def start(profile, extra=()):
        card = card_image(profile)
        d = tempfile.mkdtemp(prefix="otp-card-")
        card.install(d)
        dev = SimDevice(dir=d, args=FAST + card.sim_args + list(extra))
        devs.append(dev)
        dev.launch(60)
        return dev, card

    yield start
    failed = getattr(request.node, "rep_call", None) is not None and request.node.rep_call.failed
    for dev in devs:
        if failed:
            out = os.path.join(ARTIFACTS, request.node.name)
            os.makedirs(out, exist_ok=True)
            try:
                dev.screenshot(os.path.join(out, "panel.png"))
            except Exception:
                pass
        dev.stop()
        if failed and os.path.exists(dev.log_path):
            shutil.copy(dev.log_path, os.path.join(out, "console.log"))
        shutil.rmtree(dev.dir, ignore_errors=True)


def link_to(dev):
    return LinkClient(paired_phone(dev)).hello(timeout=15)


def listed(link, kind, timeout=10):
    pages = link.request(list=pb.ListRequest(kind=kind), all_pages=True, timeout=timeout)
    for p in pages:
        assert p.WhichOneof("body") in ("file_list", "tile_list"), p
    if pages[0].WhichOneof("body") == "tile_list":
        return [t.h3 for p in pages for t in p.tile_list.tiles]
    return {f.name: f.size for p in pages for f in p.file_list.files}


def digest(path):
    return hashlib.sha1(open(path, "rb").read()).hexdigest()


def rides_on(dev):
    return set(os.listdir(os.path.join(dev.sd, "rides")))


def fit_problems(data):
    """Why `data` isn't a whole FIT activity (None if it is): the header's
    data_size must account for the file, and the trailing CRC must match."""
    if len(data) < 14 or data[8:12] != b".FIT":
        return "no FIT header (%d bytes)" % len(data)
    size = struct.unpack_from("<I", data, 4)[0]
    if len(data) != data[0] + size + 2:
        return "header says %d data bytes, the file holds %d" % (size, len(data) - data[0] - 2)
    if struct.unpack("<H", data[-2:])[0] != fit_crc(data[:-2]):
        return "file CRC doesn't match"
    return None


# ---- booting ------------------------------------------------------------------------------------

@pytest.mark.parametrize("profile", PROFILES)
def test_boots_to_the_dashboard(boot_card, card_image, profile, record_property):
    """From power-on to a painted dashboard, with each card operation
    costing CARD_OP_MS. Also: boot's free-space figure is the card's."""
    card_image(profile)                   # built outside the timing
    t0 = time.time()
    dev, card = boot_card(profile, ["--sd-latency-ms", str(CARD_OP_MS)])
    tasks = time.time() - t0
    dev.wait_status(lambda s: s["screen"] == "dash" and s["paints"] > 0, timeout=60, interval=0.02)
    dash = time.time() - t0
    record_property("boot_s", round(dash, 2))
    print("\n%s: tasks started %.2f s, dashboard %.2f s" % (profile, tasks, dash))
    assert dash < BOOT_BOUND_S[profile], "%s card: %.1f s to the dashboard" % (profile, dash)

    line = dev.wait_log(r"sd: ready, \d+ MB free", since=0)
    said = int(re.search(r"(\d+) MB free", line).group(1))
    u = dev.sd_usage()
    assert abs(said - (u["total_bytes"] - u["used_bytes"]) // (1 << 20)) <= 1, (line, u)
    time.sleep(2)   # the UI task's start-up card work (recovery, sd update)
    assert not [l for l in dev.log() if "SD ACCESS WITHOUT sdLock" in l]
    st = dev.status()
    assert st["screen"] == "dash" and not st["ride"]["recording"]


# ---- the app's lists ----------------------------------------------------------------------------

@pytest.mark.parametrize("profile", [
    "empty", "typical", "nearly_full",
    pytest.param("heavy", marks=[SLOW, pytest.mark.xfail(strict=True, reason=(
        "BUG: LIST_RIDES stops at LIST_MAX=512 files (link.cpp) with nothing in FileList to say so — "
        "the device only logs 'link: list 1 cut at 512 files'. The walk is in directory order, oldest "
        "first on FAT, so on a card with >512 rides the app never sees the newest ones."))]),
])
def test_app_ride_list_matches_the_card(boot_card, profile):
    dev, card = boot_card(profile)
    link = link_to(dev)
    got = listed(link, pb.LIST_RIDES)
    assert got == card.rides, "app sees %d rides, the card has %d" % (len(got), len(card.rides))


@pytest.mark.parametrize("profile", PROFILES)
def test_app_route_workout_and_log_lists_match_the_card(boot_card, profile):
    dev, card = boot_card(profile)
    link = link_to(dev)
    assert listed(link, pb.LIST_ROUTES) == card.routes
    assert listed(link, pb.LIST_WORKOUTS) == card.workouts
    logs = listed(link, pb.LIST_LOGS)
    own = set(logs) - set(card.logs)     # today's log (or pending.log): the device's own
    assert {n: logs[n] for n in card.logs} == card.logs
    assert len(own) <= 2 and all(n.endswith(".log") for n in own), own


@pytest.mark.parametrize("profile", [
    "empty", "typical", "nearly_full",
    pytest.param("heavy", marks=[SLOW, pytest.mark.xfail(strict=True, reason=(
        "BUG: LIST_MAP_TILES stops at TILE_LIST_MAX=4096 (ble_server.cpp lockTileList) with no sign "
        "to the app — the silent truncation that comment says it fixed at 512, moved to 4096. The app "
        "keeps re-sending tiles the device already holds."))]),
])
def test_app_tile_list_matches_the_card(boot_card, profile):
    dev, card = boot_card(profile)
    link = link_to(dev)
    got = listed(link, pb.LIST_MAP_TILES, timeout=30)
    assert len(got) == len(set(got))
    assert set(got) == set(card.tiles), "app sees %d tiles, the card has %d" % (len(got), len(card.tiles))


# ---- the panel ------------------------------------------------------------------------------------

def list_rows(dev, path):
    """The list rows of a screenshot (between the status bar and the pager)."""
    dev.screenshot(path)
    px = read_png(path)
    return px[64 * 540:852 * 540]


@pytest.mark.parametrize("profile", PROFILES)
def test_ride_history_pages_through_the_card(boot_card, profile, tmp_path):
    """Ride History on the panel: the pager steps through every page of the
    newest HISTORY_MAX rides, and no further."""
    dev, card = boot_card(profile)
    want_pages = max(1, -(-min(len(card.rides), HISTORY_MAX) // ROWS))
    dev.console("screen menu")
    time.sleep(0.5)
    dev.tap(*HISTORY_ROW)
    time.sleep(1)
    assert dev.status()["screen"] == "other"
    shot = str(tmp_path / "page.png")
    rows = list_rows(dev, shot)
    pages = 1
    while pages <= want_pages + 1:
        paints = dev.status()["paints"]
        dev.tap(*PAGER_DOWN)
        try:   # a tap past the last page changes nothing, and nothing is painted
            dev.wait_status(lambda s: s["paints"] > paints, timeout=1.5, interval=0.05)
        except TimeoutError:
            pass
        nxt = list_rows(dev, shot)
        if nxt == rows:
            break
        rows = nxt
        pages += 1
    assert pages == want_pages, "%d pages for %d rides" % (pages, len(card.rides))


# ---- recording ------------------------------------------------------------------------------------

@pytest.mark.parametrize("profile", PROFILES)
def test_a_ride_records_saves_and_downloads(boot_card, profile):
    dev, card = boot_card(profile)
    link = link_to(dev)
    before = rides_on(dev)
    record_ride(dev, seconds=40)
    new = rides_on(dev) - before
    assert len(new) == 1, new
    name = new.pop()
    path = os.path.join(dev.sd, "rides", name)
    data = open(path, "rb").read()
    assert fit_problems(data) is None, fit_problems(data)
    got = link.download(pb.SOURCE_RIDE, name, timeout=60)
    assert got == data
    check_fit(got)
    for n in random.Random(profile).sample(sorted(card.rides), min(20, len(card.rides))):
        assert digest(os.path.join(dev.sd, "rides", n)) == digest(os.path.join(card.sd, "rides", n)), n
    if len(card.rides) < LIST_MAX:
        assert name in listed(link, pb.LIST_RIDES)


@SLOW
@pytest.mark.xfail(strict=True, reason=(
    "BUG: the ride just recorded on a card with >512 rides never reaches the app: LIST_RIDES is cut "
    "at 512 files in directory order (oldest first), so the newest ride is past the cut."))
def test_the_newest_ride_reaches_the_app_on_a_heavy_card(boot_card):
    dev, card = boot_card("heavy")
    link = link_to(dev)
    before = rides_on(dev)
    record_ride(dev, seconds=20)
    name = (rides_on(dev) - before).pop()
    assert name in listed(link, pb.LIST_RIDES)


# ---- a full card ----------------------------------------------------------------------------------

def ride_until_full(dev, seconds):
    """Start a ride, fill the card behind it (the ride file keeps the one
    cluster it has), ride on for `seconds` of device time and save."""
    dev.gps_track(line_track(37.77, -122.45, 30), speed_kmh=30)
    dev.wait_status(lambda s: s["ride"]["gps_fix"] and s["ride"]["time_valid"], timeout=15)
    before = rides_on(dev)
    dev.set_speed(10)
    dev.button("boot")
    dev.wait_status(lambda s: s["ride"]["recording"], timeout=10)
    seq = dev.log_seq()
    assert dev.fill_card(0)["free_bytes"] == 0
    dev.wait_status(lambda s: s["ride"]["elapsed_s"] >= seconds, timeout=seconds / 10 * 3 + 30, interval=0.5)
    dev.button("boot")
    dev.wait_status(lambda s: s["screen"] == "other", timeout=10)
    dev.tap(*SAVE_BUTTON)
    dev.wait_status(lambda s: not s["ride"]["recording"], timeout=20)
    dev.set_speed(1)
    new = rides_on(dev) - before
    assert len(new) == 1, new
    return new.pop(), seq


@pytest.fixture(scope="module")
def full_ride(request, card_image):
    """One ride on the nearly-full card (4 KB clusters) that runs out of
    space ~150 s in and carries on to 300 s. Shared by the tests below."""
    if not request.config.getoption("--slow"):
        pytest.skip("slow: run with --slow")
    card = card_image("nearly_full")
    d = tempfile.mkdtemp(prefix="otp-card-")
    card.install(d)
    dev = SimDevice(dir=d, args=FAST + card.sim_args)
    dev.launch(60)
    try:
        name, seq = ride_until_full(dev, 300)
        out = dict(dev=dev, card=card, name=name, log=dev.log(since=seq),
                   data=open(os.path.join(dev.sd, "rides", name), "rb").read())
        yield out
    finally:
        dev.stop()
        shutil.rmtree(d, ignore_errors=True)


@SLOW
def test_a_card_filling_mid_ride_keeps_the_device_and_earlier_rides(full_ride):
    """No crash, no hang: the ride runs to the end and stops when asked,
    the earlier rides are untouched, and the device boots again."""
    dev, card = full_ride["dev"], full_ride["card"]
    assert not [l for l in full_ride["log"] if "[sim] reboot" in l]
    for n in card.rides:
        assert digest(os.path.join(dev.sd, "rides", n)) == digest(os.path.join(card.sd, "rides", n)), n
    dev.reboot()
    dev.wait_status(lambda s: s["screen"] in ("dash", "other"), timeout=20)
    assert dev.sd_usage()["free_bytes"] == 0


@SLOW
@pytest.mark.xfail(strict=True, reason=(
    "BUG: a ride that fills the card is saved as a corrupt FIT, and logged 'ride saved'. "
    "FitWriter::writeBytes ignores short writes and keeps counting, so finish() stamps the header "
    "with data_size for records that were never written, its tail and CRC don't fit, and the file "
    "ends wherever the card did. Next boot calls it an interrupted ride."))
def test_a_ride_that_fills_the_card_is_saved_as_a_valid_fit(full_ride):
    assert fit_problems(full_ride["data"]) is None, fit_problems(full_ride["data"])


@SLOW
@pytest.mark.xfail(strict=True, reason=(
    "BUG: nobody is told the card filled mid-ride: the recorder says nothing (only diag's own "
    "'SD open/short write FAILED' every 30 s), the end-of-ride screen offers SAVE as usual, and "
    "the log says 'ride saved'."))
def test_the_rider_is_told_when_the_card_fills_mid_ride(full_ride):
    told = [l for l in full_ride["log"]
            if re.search(r"(rec|ride).*(card full|no space|write fail|not saved|ENOSPC)", l, re.I)]
    assert told, "no recorder message about the full card"


def full_device(boot_card):
    dev, card = boot_card("nearly_full")
    link = link_to(dev)
    assert dev.fill_card(0)["free_bytes"] == 0
    return dev, card, link


@pytest.mark.xfail(strict=True, reason=(
    "BUG: a route uploaded to a full card is reported RESULT_OK and loaded, but routes.cpp "
    "loadFromMemory ignores the failed write: /routes/<name> is left empty and the route is gone "
    "after a reboot."))
def test_a_route_upload_to_a_full_card_is_refused(boot_card):
    dev, card, link = full_device(boot_card)
    gpx = cards.gpx_route("Full", 37.77, -122.42, 20, random.Random(1))
    with pytest.raises(LinkError):
        link.upload(pb.TARGET_ROUTE, "Full.gpx", gpx, timeout=60)


def a_new_tile():
    cell, box = cards.h3_cells((37.0, -121.0, 37.05, -120.95))[0]   # a prefix no profile uses
    return cell, cards.sample_tile(cell, box)


def test_a_tile_upload_to_a_full_card_fails_and_leaves_nothing(boot_card):
    dev, card, link = full_device(boot_card)
    cell, tile = a_new_tile()
    with pytest.raises(LinkError) as e:
        link.upload(pb.TARGET_MAP_TILE, cell, tile, timeout=60)
    assert e.value.code in ("RESULT_IO_ERROR", "RESULT_PARSE_ERROR")
    assert not os.path.exists(dev.sd + cards.tile_path(cell))
    assert dev.status()["screen"] == "dash"


@pytest.mark.xfail(strict=True, reason=(
    "BUG: a tile that doesn't fit is answered RESULT_PARSE_ERROR ('not a usable tile, or SD write "
    "failed'), not RESULT_IO_ERROR: the app can't tell a full card from a bad tile."))
def test_a_tile_upload_to_a_full_card_says_io_error(boot_card):
    dev, card, link = full_device(boot_card)
    cell, tile = a_new_tile()
    with pytest.raises(LinkError) as e:
        link.upload(pb.TARGET_MAP_TILE, cell, tile, timeout=60)
    assert e.value.code == "RESULT_IO_ERROR", e.value


@pytest.mark.xfail(strict=True, reason=(
    "BUG: map_store::saveTile remembers a tile directory as made ('ensured') even when its mkdir "
    "failed on the full card, so once space is freed every tile for that directory still fails "
    "(open: ENOENT) until the device restarts."))
def test_tiles_save_again_once_space_is_freed(boot_card):
    dev, card, link = full_device(boot_card)
    cell, tile = a_new_tile()
    with pytest.raises(LinkError):
        link.upload(pb.TARGET_MAP_TILE, cell, tile, timeout=60)
    os.remove(os.path.join(dev.sd, "filler.bin"))
    assert dev.sd_usage()["free_bytes"] > 1 << 20
    link.upload(pb.TARGET_MAP_TILE, cell, tile, timeout=60)
    assert os.path.exists(dev.sd + cards.tile_path(cell))


@pytest.mark.xfail(strict=True, reason=(
    "BUG: a workout upload to a full card is refused (RESULT_IO_ERROR) but leaves an empty "
    "/workouts/<name> behind, which LIST_WORKOUTS and the picker then offer."))
def test_a_workout_upload_to_a_full_card_is_refused_and_leaves_nothing(boot_card):
    dev, card, link = full_device(boot_card)
    with pytest.raises(LinkError) as e:
        link.upload(pb.TARGET_WORKOUT, "new.erg", cards.erg_workout("new", random.Random(2)), timeout=60)
    assert e.value.code == "RESULT_IO_ERROR"
    assert "new.erg" not in listed(link, pb.LIST_WORKOUTS)


def test_a_firmware_upload_to_a_full_card_is_refused(boot_card):
    dev, card, link = full_device(boot_card)
    with pytest.raises(LinkError) as e:
        link.upload(pb.TARGET_FIRMWARE, "firmware.bin", b"\xE9" + os.urandom(200 * 1024 - 1), timeout=60)
    assert e.value.code == "RESULT_IO_ERROR"
    assert not os.path.exists(os.path.join(dev.sd, "firmware.bin"))
    assert dev.status()["screen"] == "dash"


def test_deleting_rides_from_the_app_frees_space(boot_card):
    """On a full card: deleting rides gives their clusters back, they leave
    the list, and the card takes new files again."""
    dev, card, link = full_device(boot_card)
    gone = sorted(card.rides)[:10]
    cl = dev.sd_usage()["cluster_bytes"]
    for n in gone:
        r = link.request(delete_file=pb.DeleteRequest(kind=pb.LIST_RIDES, name=n))
        assert r.WhichOneof("body") == "result" and r.result.code == pb.RESULT_OK, r
    freed = sum(-(-card.rides[n] // cl) * cl for n in gone)
    free = dev.sd_usage()["free_bytes"]
    assert freed - 8 * cl <= free <= freed, (free, freed)   # diag's log may take a cluster or two back
    left = listed(link, pb.LIST_RIDES)
    assert not set(gone) & set(left)
    assert len(left) == len(card.rides) - len(gone)
    link.upload(pb.TARGET_WORKOUT, "after.erg", cards.erg_workout("after", random.Random(3)), timeout=30)
    assert "after.erg" in listed(link, pb.LIST_WORKOUTS)
