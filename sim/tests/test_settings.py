"""Settings (src/settings.cpp, the link's Settings, the Settings screen, the
side key) and the idle auto-shutdown: every value the app can set, what the
device does with values out of range, that each one is kept across a reboot
and reaches the hardware or the panel, that an edit on the device reaches
the app, and when the device may switch itself off."""
import os
import time

import pytest

from conftest import TIME_SCALE
from otpsim import LinkClient, pb
from test_regressions_ui import glass, pair_sensor

PWM = {0: 0, 1: 12, 2: 90, 3: 230}     # kBacklightPWM (ui_dashboard.cpp)

# The Settings screen (ui_render.h): 108 px rows from y 64; the stepper's
# -/+ targets and the switches' hit rects.
ROW = lambda r: 64 + r * 108 + 54
MINUS_X, PLUS_X = 192 + 44, 412 + 44
TOGGLE_X, UNITS_TOGGLE_X = 380 + 60, 332 + 84


def reconnect(device, phone):
    phone.wait_disconnected(10)
    phone.connect()
    assert phone.wait_security(10)["encrypted"]
    return LinkClient(phone).hello()


def tap(device, x, y):
    device.tap(x, y)
    time.sleep(0.45)


# -- over the link ------------------------------------------------------------------------

def test_every_setting_round_trips_and_survives_a_reboot(device, phone, link):
    want = dict(ftp_w=285, tz_min=-420, use_miles=True, backlight=2, clock_24h=False, usb_drive=True,
                wheel_mm=2096)
    r = link.request(set_settings=pb.Settings(**want))
    assert r.WhichOneof("body") == "settings"
    for k, v in want.items():
        assert getattr(r.settings, k) == v, k
    st = device.status()["settings"]
    assert (st["ftp"], st["tz_min"], st["miles"], st["backlight"], st["clock24h"], st["wheel_mm"]) == \
        (285, -420, True, 2, False, 2096)
    device.reboot()
    link2 = reconnect(device, phone)
    s = link2.wait_push("settings", timeout=5)   # pushed after Hello: what the app shows
    for k, v in want.items():
        assert getattr(s, k) == v, k
    got = link2.request(get_settings=pb.GetSettings()).settings
    assert got == s


def test_a_partial_edit_changes_only_what_it_names(device, link):
    link.request(set_settings=pb.Settings(ftp_w=260, use_miles=True))
    r = link.request(set_settings=pb.Settings(tz_min=60))
    assert (r.settings.ftp_w, r.settings.use_miles, r.settings.tz_min) == (260, True, 60)


@pytest.mark.parametrize("field,sent,applied", [
    ("ftp_w", 10, 50), ("ftp_w", 9000, 500),
    ("tz_min", -2000, -720), ("tz_min", 2000, 840),
    ("backlight", 7, 3), ("backlight", 1 << 20, 3),
])
def test_out_of_range_values_are_clamped_and_the_app_is_told(device, link, field, sent, applied):
    """The answer is the value now in force, so the app can show it instead
    of what was typed."""
    r = link.request(set_settings=pb.Settings(**{field: sent}))
    assert getattr(r.settings, field) == applied


@pytest.mark.parametrize("mm", [100, 799, 3501, 100000, 4_000_000_000])
def test_an_impossible_wheel_is_refused_not_clamped(device, link, mm):
    """A typo of 210 mm must not become an 800 mm wheel: out of range is
    refused and the wheel stays what it was."""
    link.request(set_settings=pb.Settings(wheel_mm=2150))
    r = link.request(set_settings=pb.Settings(wheel_mm=mm))
    assert r.settings.wheel_mm == 2150
    assert device.status()["settings"]["wheel_mm"] == 2150


def test_the_backlight_from_the_app_lights_the_panel(device, link):
    """The app's setting writes NVS; the UI task drives the PWM from it."""
    for level in (3, 1, 0, 2):
        link.request(set_settings=pb.Settings(backlight=level))
        device.wait_status(lambda s: s["backlight_pwm"] == PWM[level], timeout=3)


def test_the_side_key_steps_the_backlight_and_keeps_it(device):
    """Off → low → medium → bright → off, and the level is kept."""
    assert device.status()["backlight_pwm"] == 0
    for level in (1, 2, 3, 0, 1):
        device.button("side")
        st = device.wait_status(lambda s: s["backlight_pwm"] == PWM[level], timeout=3)
        assert st["settings"]["backlight"] == level
        time.sleep(0.3)
    device.reboot()
    st = device.wait_status(lambda s: s["backlight_pwm"] == PWM[1], timeout=5)


@pytest.mark.xfail(strict=True, reason=(
    "BUG: the side key's backlight step (ui_dashboard.cpp cycleBacklight) saves the level but, unlike the "
    "Settings screen's -/+ (which call ble_server::pushSettingsToPhone), never tells the app — the app's "
    "backlight control keeps showing the old level until it reconnects."))
def test_the_side_key_backlight_reaches_the_app(device, link):
    link.drain_pushes()
    device.button("side")
    s = link.wait_push("settings", timeout=5)
    assert s.backlight == 1


def test_an_edit_on_the_settings_screen_reaches_the_app(device, link):
    """FTP +, backlight +/-, the units switch: each lands at once and is
    pushed to the app (the app's screens follow the device)."""
    tz = device.status()["settings"]["tz_min"]
    device.console("screen settings")
    time.sleep(1.2)
    link.drain_pushes()
    tap(device, PLUS_X, ROW(0))                          # FTP +5
    s = link.wait_push("settings", lambda s: s.ftp_w == 255, timeout=5)
    tap(device, PLUS_X, ROW(2))                          # backlight +
    s = link.wait_push("settings", lambda s: s.backlight == 1, timeout=5)
    device.wait_status(lambda s: s["backlight_pwm"] == PWM[1], timeout=3)
    tap(device, MINUS_X, ROW(2))
    tap(device, MINUS_X, ROW(2))                         # clamped at off, not wrapped
    s = link.wait_push("settings", lambda s: s.backlight == 0, timeout=5)
    assert device.status()["settings"]["backlight"] == 0
    tap(device, UNITS_TOGGLE_X, ROW(3))                  # km → mi
    link.wait_push("settings", lambda s: s.use_miles, timeout=5)
    tap(device, 100, ROW(1))                             # beside the stepper: nothing, and we stay
    assert device.status()["screen"] == "other"
    assert device.status()["settings"]["tz_min"] == tz


def test_show_offline_is_the_devices_and_the_app_hears_it(device, link):
    """'Show offline sensors' lives on the device only: off, a field whose
    sensor is not live leaves the page and the rest re-pack; the app's live
    frames carry the switch so its copy of the page matches; it is kept."""
    link.expect_result(live_subscribe=pb.LiveSubscribe(topics=[pb.LIVE_TOPIC_RIDE]))
    link.wait_push("live", lambda f: f.ride.show_offline, timeout=5)
    device.console("screen settings")
    time.sleep(1.2)
    tap(device, TOGGLE_X, ROW(5))
    link.wait_push("live", lambda f: f.HasField("ride") and not f.ride.show_offline, timeout=5)
    assert not device.status()["settings"]["show_offline"]
    device.reboot()
    assert not device.status()["settings"]["show_offline"]


# -- what the panel shows ----------------------------------------------------------------------

def test_units_change_the_speed_on_the_panel(device, link, tmp_path):
    link.request(set_dash=pb.DashConfig(text="speed hero\ndistance large\n"))
    device.gps(45.70, -121.52, speed_kmh=36)
    device.wait_status(lambda s: s["ride"]["gps_fix"] and s["ride"]["speed_kmh"] > 30, timeout=10)
    time.sleep(1.5)
    km = glass(device, tmp_path, "km")
    link.request(set_settings=pb.Settings(use_miles=True))
    time.sleep(1.5)
    mi = glass(device, tmp_path, "mi")
    assert km.region(0, 64, 540, 520) != mi.region(0, 64, 540, 520), "the speed reads the same in mph"
    link.request(set_settings=pb.Settings(use_miles=False))
    time.sleep(1.5)
    assert glass(device, tmp_path, "km2").region(0, 64, 540, 520) == km.region(0, 64, 540, 520)


def test_the_clock_setting_changes_the_status_bar(device, link, tmp_path):
    # An afternoon hour, whatever the time now: the 12-hour clock carries no
    # AM/PM, so a morning reads the same either way. The device clock is the
    # phone's (Hello); the time zone moves it to 15:xx local.
    utc_h = time.gmtime(device.status()["wall_unix"]).tm_hour
    tz = ((15 - utc_h + 12) % 24 - 12) * 60
    if tz < -720:
        tz += 1440
    link.request(set_settings=pb.Settings(clock_24h=True, tz_min=tz))
    time.sleep(1.5)
    h24 = glass(device, tmp_path, "24h")
    link.request(set_settings=pb.Settings(clock_24h=False))
    time.sleep(1.5)
    h12 = glass(device, tmp_path, "12h")
    assert h24.region(0, 0, 180, 60) != h12.region(0, 0, 180, 60), "15:00 and 3:00 PM look the same"


# -- auto-sleep ------------------------------------------------------------------------------

IDLE_S = 600   # AUTO_SLEEP_MS


def idle_for(device, seconds, factor=60):
    """Let `seconds` of device time pass, quickly."""
    device.set_speed(factor)
    time.sleep(seconds / factor * TIME_SCALE)
    if not device.powered_off():
        device.set_speed(1)


def _trainer_workout_without_a_phone(device, phone, link):
    pair_sensor(device, link, "power", watts=200, cadence=90)
    device.wait_status(lambda s: s["ride"]["power"] == 200, timeout=30)
    os.makedirs(os.path.join(device.sd, "workouts"), exist_ok=True)
    with open(os.path.join(device.sd, "workouts", "long.erg"), "w") as f:
        f.write("[COURSE HEADER]\nMINUTES WATTS\n[END COURSE HEADER]\n[COURSE DATA]\n0 200\n60 200\n"
                "[END COURSE DATA]\n")
    assert any("loaded LONG" in l for l in device.console("workout load long.erg"))
    device.console("workout start")
    phone.disconnect()
    device.wait_status(lambda s: not s["phone_connected"], timeout=5)


@pytest.mark.xfail(strict=True, reason=(
    "BUG: the idle auto-shutdown (ui_dashboard.cpp task(), AUTO_SLEEP_MS) is held off by a ride, navigation, "
    "movement, a phone or a running STOPWATCH, but not by a running structured workout: a rider on a trainer "
    "(nothing moves, the workout clock runs without recording a ride, the phone put away) has the device switch "
    "itself off ten minutes into the session, and the workout is lost with it (it is RAM only)."))
def test_a_running_workout_keeps_it_on(device, phone, link):
    _trainer_workout_without_a_phone(device, phone, link)
    idle_for(device, IDLE_S + 120)
    assert not device.powered_off()
