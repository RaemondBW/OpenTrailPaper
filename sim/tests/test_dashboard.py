"""The dashboard's pages and layout (src/dash_layout.cpp, src/dash_config.cpp,
the link's GetDash / SetDash / ShowPage, ui_dashboard's carousel): what the
app sends, what the device keeps and shows, how the rider moves between
pages, and the config file on the card that is the one copy of it all."""
import os
import re
import time

import pytest

from otpsim import pb
from test_regressions_ui import glass

FIELDS = ["speed", "avgspeed", "power3s", "power", "hr", "cadence", "distance", "ridetime",
          "movingtime", "climb", "grade", "altitude", "battery", "sats", "clock", "routeleft", "radar"]
CFG = "/config/dashboard.cfg"


def set_dash(link, text):
    r = link.request(set_dash=pb.DashConfig(text=text))
    assert r.WhichOneof("body") == "dash", r
    return r.dash.text


def parse(text):
    """The device's layout text as [(kind, [item lines])], the map strip apart:
    (pages, map_fields). A page's items are their whitespace-normalised lines."""
    pages, cur, strip = [], ("fields", []), None
    for line in text.split("\n"):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        tok = line.split()
        if tok[0] == "page":
            if cur[0] != "fields" or cur[1]:
                pages.append(cur)
            cur = (" ".join(tok[1:]) or "fields", [])
        elif tok[0] == "map":
            strip = tok[1:]
        else:
            cur[1].append(" ".join(tok))
    if cur[0] != "fields" or cur[1]:
        pages.append(cur)
    return pages, strip


def wait_pages(device, n):
    return device.wait_status(lambda s: s["page_count"] == n, timeout=5)


# -- what the device keeps ----------------------------------------------------------------

def test_every_field_size_and_placement_round_trips(device, link, tmp_path):
    """All seventeen fields across two pages, every size, half rows and the
    radar's side column with its height and position: the device answers
    with exactly that layout (in its own spelling), and draws each page."""
    sizes = ["small", "medium", "large", "hero"]
    page1 = ["%s %s%s" % (f, sizes[i % 4] if i else "hero", " half" if 2 <= i <= 5 else "")
             for i, f in enumerate(FIELDS[:9])]
    page2 = ["%s %s%s" % (f, sizes[i % 3], " half" if i in (1, 2) else "") for i, f in enumerate(FIELDS[9:16])]
    page2.append("radar small vertical height=75 position=bottom")
    reply = set_dash(link, "\n".join(page1) + "\npage\n" + "\n".join(page2) + "\n")
    pages, strip = parse(reply)
    assert [k for k, _ in pages] == ["fields", "fields", "map"]
    assert pages[0][1] == page1
    assert pages[1][1] == page2
    assert strip == ["speed", "distance", "ridetime"]
    wait_pages(device, 3)
    assert parse(device.status()["dash"]) == (pages, strip)
    for i in range(2):
        link.tell(show_page=pb.ShowPage(page=i))
        device.wait_status(lambda s: s["page"] == i and s["screen"] == "dash", timeout=5)
        time.sleep(1.2)
        g = glass(device, tmp_path, "page%d" % i)
        assert g.ink(0, 64, 540, 960) > 0.02, "page %d drew nothing" % (i + 1)


def test_the_map_strip_is_part_of_the_layout(device, link):
    """`map <f> <f> <f>`: the map screen's three cells. The radar is a side
    column, never a strip cell, and a typo keeps that slot's default."""
    pages, strip = parse(set_dash(link, "map hr power3s clock\nspeed hero\n"))
    assert strip == ["hr", "power3s", "clock"]
    pages, strip = parse(set_dash(link, "map radar nonsense grade\nspeed hero\n"))
    assert strip == ["speed", "distance", "grade"]


def test_the_map_page_goes_where_the_rider_put_it(device, link):
    """The map is a page like the others: second here, and there is only
    ever one — a second `page map` is dropped."""
    pages, _ = parse(set_dash(link, "speed hero\npage map\npage stopwatch\npage workout\npage map\npage music\n"))
    assert [k for k, _ in pages] == ["fields", "map", "stopwatch", "workout", "music"]
    wait_pages(device, 5)


def test_typos_cost_one_line_not_the_layout(device, link):
    """A hand-edited line the device can't read is skipped; the rest of the
    page stands. A page left with nothing is dropped."""
    pages, _ = parse(set_dash(link, "speed hero\nsped large\nhr wobbly half\npage\nnonsense\npage\ncadence\n"))
    assert pages == [("fields", ["speed hero", "hr medium half"]), ("fields", ["cadence medium"]), ("map", [])]


def test_a_layout_with_only_the_map_is_refused_and_the_old_one_kept(device, link):
    set_dash(link, "speed hero\n")
    for text in ("page map\n", "# nothing but a comment\n", "", "page\npage\n"):
        r = link.request(set_dash=pb.DashConfig(text=text))
        assert r.WhichOneof("body") == "result" and r.result.code == pb.RESULT_PARSE_ERROR, text
    assert parse(device.status()["dash"])[0][0] == ("fields", ["speed hero"])


def test_seven_content_pages_plus_the_map_fit(device, link):
    """DeviceHello.dash_max_pages: 7 content pages, the map on top."""
    assert link.device_hello.dash_max_pages == 7
    text = "".join(("page\n" if i else "") + "%s hero\n" % FIELDS[i] for i in range(7))
    pages, _ = parse(set_dash(link, text))
    assert len(pages) == 8 and pages[-1][0] == "map"
    wait_pages(device, 8)


@pytest.mark.xfail(strict=True, reason=(
    "BUG: dashParsePages (src/dash_layout.cpp commit()) marks the map as seen even when DASH_MAX_PAGES is "
    "already full and it is not kept, and the end-of-parse append also needs a free slot — so a layout with "
    "8 content pages (a hand-edited file, or older firmware's app) loses the MAP page entirely: the Home key "
    "never reaches the map. The header promises 'there is always exactly one'."))
def test_too_many_pages_never_cost_the_map(device, link):
    text = "".join(("page\n" if i else "") + "%s hero\n" % FIELDS[i] for i in range(8)) + "page map\n"
    pages, _ = parse(set_dash(link, text))
    assert "map" in [k for k, _ in pages]


@pytest.mark.xfail(strict=True, reason=(
    "BUG: the device's readback of a full layout (7 pages x 12 fields, which both apps' editors allow) is cut "
    "off: dashSerializePages (src/dash_layout.cpp) pads every line ('%-10s %-6s') so the text outgrows "
    "DashConfig.text (2048, proto/link.options) and sendDash sends the truncated buffer — the app's editor "
    "reloads it with the last page cut mid-line and the map gone, and saving again loses them on the device."))
def test_a_full_layout_reads_back_whole(device, link):
    fields = FIELDS[:12]
    sent = [[f + " small half" for f in fields] for _ in range(7)]
    text = "page\n".join("\n".join(p) + "\n" for p in sent)
    assert len(text) <= link.device_hello.dash_max_text
    pages, _ = parse(set_dash(link, text))
    assert [items for k, items in pages if k == "fields"] == sent
    assert pages[-1][0] == "map"


# -- the file on the card -------------------------------------------------------------------

def test_the_layout_is_the_file_on_the_card_and_survives_a_reboot(device, link):
    text = "power3s hero\nhr half\ncadence half\npage music\n"
    set_dash(link, text)
    assert open(os.path.join(device.sd, CFG.lstrip("/"))).read() == text
    device.reboot()
    pages, _ = parse(device.status()["dash"])
    assert [k for k, _ in pages] == ["fields", "music", "map"]


def test_a_first_boot_writes_the_default_layout_out_to_be_edited(device):
    path = os.path.join(device.sd, CFG.lstrip("/"))
    text = open(path).read()
    assert text.startswith("# OpenTrailPaper dashboard layout")
    assert parse(text)[0][0] == ("fields", ["power3s hero", "hr medium half", "cadence medium half",
                                             "ridetime medium half", "distance medium half"])


def test_an_unusable_file_falls_back_to_the_default_and_says_so(device):
    path = os.path.join(device.sd, CFG.lstrip("/"))
    with open(path, "w") as f:
        f.write("this is not a layout\n")
    device.reboot()
    assert parse(device.status()["dash"])[0][0][1][0] == "power3s hero"
    device.wait_log(r"dash: /config/dashboard.cfg has no usable fields", since=0, timeout=2)


@pytest.mark.xfail(strict=True, reason=(
    "BUG: dash_config::begin() runs once, in ui_dashboard::begin() at boot. A card that only mounts after boot "
    "(the late-mount path in ui_dashboard.cpp task(): routes, segments and maps are loaded, the layout is not) "
    "leaves the rider on the built-in default dashboard for the whole session, their own layout ignored."))
def test_a_card_that_mounts_late_brings_its_layout(device, link):
    set_dash(link, "speed hero\npage stopwatch\n")
    device.set_sd(False)
    device.reboot()
    device.wait_log(r"boot mount failed", since=0, timeout=5)
    device.set_speed(10)
    device.set_sd(True)
    device.wait_log(r"sd: card back", timeout=15)
    device.set_speed(1)
    time.sleep(1)
    assert parse(device.status()["dash"])[0][0] == ("fields", ["speed hero"])


@pytest.mark.xfail(strict=True, reason=(
    "BUG (or a missing feature — dash_config.h says the file is there to be edited over USB and that begin() "
    "should run 'again after a rescan', but nothing calls it): a layout edited in /config/dashboard.cfg while "
    "a computer holds the card is not read when the card is ejected (ui_dashboard.cpp task() rescans the maps "
    "and applies firmware.bin, not the layout); the rider sees the old dashboard until the next reboot."))
def test_a_layout_edited_over_usb_applies_when_the_card_is_ejected(device):
    device.usb_host(True)
    with open(os.path.join(device.sd, CFG.lstrip("/")), "w") as f:
        f.write("speed hero\nclock large\n")
    device.usb_host(False)
    device.wait_status(lambda s: parse(s["dash"])[0][0] == ("fields", ["speed hero", "clock large"]), timeout=5)


def test_a_layout_sent_mid_ride_waits_for_the_ride_to_end(device, link):
    """SetDash writes the card, and the card belongs to the recorder while a
    ride records: the device holds the layout (one at a time — a second is
    BUSY) and applies it when the ride is saved."""
    from test_regressions_ui import start_recording
    from test_rides import SAVE_BUTTON
    start_recording(device)
    link.drain_pushes()
    with pytest.raises(TimeoutError):
        link.request(set_dash=pb.DashConfig(text="speed hero\nclock\n"), timeout=2)
    r = link.request(set_dash=pb.DashConfig(text="hr hero\n"))
    assert r.WhichOneof("body") == "result" and r.result.code == pb.RESULT_BUSY
    assert "power3s" in device.status()["dash"]          # not yet
    device.button("boot")
    device.wait_status(lambda s: s["screen"] == "other", timeout=10)
    device.tap(*SAVE_BUTTON)
    device.wait_status(lambda s: not s["ride"]["recording"], timeout=20)
    late = link.wait_push("dash", timeout=10)            # the held request's answer
    assert parse(late.text)[0][0] == ("fields", ["speed hero", "clock medium"])
    assert parse(device.status()["dash"])[0][0] == ("fields", ["speed hero", "clock medium"])


# -- moving between pages -------------------------------------------------------------------

def test_the_home_key_steps_through_the_pages_in_order(device, link):
    """The carousel, map included wherever it sits, wrapping round."""
    set_dash(link, "speed hero\npage map\npage stopwatch\npage workout\n")
    wait_pages(device, 4)
    device.wait_status(lambda s: s["page"] == 0 and s["screen"] == "dash")
    want = [(1, "map"), (2, "dash"), (3, "dash"), (0, "dash"), (1, "map")]
    for page, screen in want:
        time.sleep(0.45)                                   # the key is debounced 400 ms
        device.button("home")
        device.wait_status(lambda s: s["page"] == page and s["screen"] == screen, timeout=3)


def test_the_app_shows_any_page_but_never_over_a_menu(device, link):
    set_dash(link, "speed hero\npage stopwatch\npage map\n")
    wait_pages(device, 3)
    link.tell(show_page=pb.ShowPage(page=2))
    device.wait_status(lambda s: s["page"] == 2 and s["screen"] == "map", timeout=3)
    link.tell(show_page=pb.ShowPage(page=1))
    device.wait_status(lambda s: s["page"] == 1 and s["screen"] == "dash", timeout=3)
    device.tap(270, 30)                                   # the status bar: the menu
    device.wait_status(lambda s: s["screen"] == "other", timeout=3)
    link.tell(show_page=pb.ShowPage(page=0))
    time.sleep(1)
    assert device.status()["screen"] == "other", "ShowPage took the panel from the menu"
    time.sleep(0.45)
    device.button("home")                                 # back out: the page the rider was on
    device.wait_status(lambda s: s["screen"] == "dash" and s["page"] == 1, timeout=3)


def test_a_layout_losing_the_page_on_screen_moves_to_the_first(device, link):
    set_dash(link, "speed hero\npage\nhr hero\npage\ncadence hero\n")
    link.tell(show_page=pb.ShowPage(page=2))
    device.wait_status(lambda s: s["page"] == 2, timeout=3)
    set_dash(link, "clock hero\n")
    device.wait_status(lambda s: s["page_count"] == 2 and s["page"] == 0 and s["screen"] == "dash", timeout=3)


def test_the_view_topic_tells_the_app_where_the_rider_is(device, link):
    set_dash(link, "speed hero\npage map\npage stopwatch\n")
    link.expect_result(live_subscribe=pb.LiveSubscribe(topics=[pb.LIVE_TOPIC_VIEW]))
    wait_pages(device, 3)
    for page, screen in ((1, pb.SCREEN_MAP), (2, pb.SCREEN_DASH)):
        time.sleep(0.45)
        device.button("home")
        f = link.wait_push("live", lambda f: f.HasField("view") and f.view.page == page, timeout=5)
        assert f.view.screen == screen and f.view.page_count == 3
    device.tap(270, 30)                                   # menu: no page
    f = link.wait_push("live", lambda f: f.view.screen == pb.SCREEN_OTHER, timeout=5)
    assert not f.view.HasField("page")
