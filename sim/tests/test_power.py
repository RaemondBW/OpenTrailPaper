"""Power: the idle auto-off, what holds it off and what restarts its
countdown, the power dialog, and the battery as the rider and the app see it.

A bike computer left on a desk must switch itself off (ui_dashboard.cpp,
AUTO_SLEEP_MS: 10 minutes without input), and must never do it under a
rider. Deep sleep exits the sim; `power_on()` is BOOT pressed afterwards.
The clock runs fast (power_util.run_to) and slows down near the moment the
test is about, so each switch-off is timed to about a second of device time."""
import os
import time

import pytest

from conftest import paired_phone
from otpsim import LinkClient, pb
from power_util import (FAST, IDLE_S, card_logs, check_finished_fit, console_text, fit_records, only_ride,
                        run_to, uptime_s)
from test_rides import SAVE_BUTTON, line_track

LAT, LON = 37.77, -122.45
SHUT_DOWN = (24 + 246, 700 + 48)     # ui_render.cpp kPowerShutdown centre
CANCEL = (24 + 246, 816 + 48)        # kPowerCancel centre
TOLERANCE_S = 30                     # the UI loop's pass at a fast clock, the poll's resolution


def assert_switched_off_after_idle(off, since):
    assert off is not None, "still on %d s after the last input" % (IDLE_S + 100)
    assert off >= since + IDLE_S - 2, "switched off %.0f s after the last input, not %d" % (off - since, IDLE_S)
    assert off <= since + IDLE_S + TOLERANCE_S, "switched off only %.0f s after the last input" % (off - since)


def stays_on(dev, minutes):
    """Run the clock `minutes` past now; the device must still be on."""
    t = uptime_s(dev)
    off = run_to(dev, t + minutes * 60)
    assert off is None, "switched itself off at %.0f s (%.0f s after the check began)" % (off, off - t)


# ---- the idle countdown ------------------------------------------------------------------

def test_an_idle_device_switches_itself_off_after_ten_minutes(device):
    """Left on a desk, the device must switch off on its own — and not
    before ten minutes. It says why in the card's log before it goes, so the
    next boot's log tells an auto-off from a crash or a power cut, and BOOT
    brings it back (a wake from deep sleep, not a crash)."""
    off = run_to(device, IDLE_S + 120, fine_from=IDLE_S - 30)
    assert_switched_off_after_idle(off, 0)
    assert "shutdown: auto-sleep (idle timeout)" in console_text(device)
    assert "shutdown: auto-sleep (idle timeout)" in card_logs(device), "the reason never reached the card"

    device.power_on()
    st = device.status()
    assert st["reset_reason"] == 8          # ESP_RST_DEEPSLEEP
    assert st["screen"] == "dash"
    assert not [n for n in os.listdir(os.path.join(device.sd, "logs")) if n.startswith("crash-")]
    # A fresh countdown on the new boot.
    assert run_to(device, IDLE_S - 30) is None


@pytest.mark.parametrize("touch", ["tap", "side key", "home key"])
def test_any_input_restarts_the_idle_countdown(device, touch):
    """A tap or a key is the rider using the device: the ten minutes start
    again from there. (A BOOT press starts a ride, which holds it on anyway.)"""
    assert run_to(device, 480) is None
    if touch == "tap":
        device.tap(270, 480)
    elif touch == "side key":
        device.button("side")
    else:
        device.button("home")
    t = uptime_s(device)
    off = run_to(device, t + IDLE_S + 120, fine_from=t + IDLE_S - 30)
    assert_switched_off_after_idle(off, t)


def test_autosleep_off_on_the_console_keeps_it_on_until_switched_back(device):
    """The bench override (`autosleep off`) holds the device on; `autosleep
    on` starts a full countdown from then, not from the last tap an hour ago."""
    assert any("OFF" in l for l in device.console("autosleep off"))
    stays_on(device, 25)
    device.console("autosleep on")
    t = uptime_s(device)
    off = run_to(device, t + IDLE_S + 120, fine_from=t + IDLE_S - 30)
    assert_switched_off_after_idle(off, t)


# ---- what holds it off -----------------------------------------------------------------------

def test_a_ride_in_progress_is_never_switched_off(device):
    """Stopped at a café with the ride recording (auto-paused, nobody
    touching it): the device must stay on, however long. Once the rider saves
    the ride, the countdown runs from the save."""
    device.gps(LAT, LON, speed_kmh=0)
    device.wait_status(lambda s: s["ride"]["gps_fix"] and s["ride"]["time_valid"], timeout=15)
    device.button("boot")
    device.wait_status(lambda s: s["ride"]["recording"], timeout=10)
    stays_on(device, 25)
    assert device.status()["ride"]["recording"]

    device.button("boot")                     # stop: the save/discard sheet
    device.wait_status(lambda s: s["screen"] == "other", timeout=10)
    device.tap(*SAVE_BUTTON)
    device.wait_status(lambda s: not s["ride"]["recording"], timeout=20)
    t = uptime_s(device)
    off = run_to(device, t + IDLE_S + 120, fine_from=t + IDLE_S - 30)
    assert_switched_off_after_idle(off, t)
    _, data = only_ride(device)
    check_finished_fit(data)


def test_riding_without_recording_keeps_it_on_and_stopping_starts_the_countdown(device):
    """Riding along with the dashboard up but no ride recording: speed is
    activity, so it stays on; once the bike stops, ten minutes later it
    switches off."""
    device.gps_track(line_track(LAT, LON, 3), speed_kmh=20, loop=True)
    device.wait_status(lambda s: s["ride"]["gps_fix"] and s["ride"]["speed_kmh"] > 10, timeout=15)
    stays_on(device, 25)
    st = device.status()
    device.gps(st["ride"]["lat"], st["ride"]["lon"], speed_kmh=0)
    device.wait_status(lambda s: s["ride"]["speed_kmh"] < 1, timeout=10)
    t = uptime_s(device)
    off = run_to(device, t + IDLE_S + 120, fine_from=t + IDLE_S - 30)
    assert_switched_off_after_idle(off, t)


def test_a_connected_phone_keeps_it_on(device, link):
    """With the app connected (a map download, a route on the way, the
    rider looking at the app's live view), the device must not vanish."""
    stays_on(device, 25)
    assert device.status()["phone_connected"]


@pytest.mark.xfail(strict=True, reason=(
    "BUG: ui_dashboard.cpp's auto-sleep check holds off only while the phone is connected and never "
    "restarts the idle countdown when the link ends, so a phone that has been connected for over ten "
    "minutes takes the device down the moment its link drops (walking out of range, iOS dropping the "
    "connection in the background): it powers off within seconds, before the phone can reconnect"))
def test_a_dropped_phone_link_does_not_switch_it_off_at_once(device, phone):
    LinkClient(phone).hello()
    stays_on(device, 15)
    device.drop_phone(0x08)                   # supervision timeout: out of range
    phone.wait_disconnected(5)
    t = uptime_s(device)
    off = run_to(device, t + 120)
    assert off is None, "powered off %.0f s after the phone's link dropped" % (off - t)


def test_a_running_stopwatch_keeps_it_on_after_the_phone_leaves(device, link, phone):
    """Started from the app, the stopwatch would lose its time if the device
    switched off: it holds the device on with nobody connected."""
    link.expect_result(live_subscribe=pb.LiveSubscribe(topics=[pb.LIVE_TOPIC_STOPWATCH]))
    link.tell(stopwatch=pb.StopwatchCommand(op=pb.STOPWATCH_OP_TOGGLE))
    link.wait_push("live", lambda f: f.HasField("stopwatch") and f.stopwatch.running, timeout=5)
    phone.disconnect()
    device.wait_status(lambda s: not s["phone_connected"], timeout=10)
    stays_on(device, 25)


@pytest.mark.xfail(strict=True, reason=(
    "BUG: the auto-sleep check in ui_dashboard.cpp ignores usb_storage::hostActive(): with the card "
    "mounted on a computer over USB (copying maps or rides, nobody touching the panel) the device "
    "deep-sleeps after ten minutes, unmounts the card (SD.end) and drops off the USB bus under the "
    "computer — an unsafe eject mid-copy"))
def test_a_computer_holding_the_card_keeps_it_on(device):
    device.usb_host(True)
    stays_on(device, 25)


# ---- the power dialog --------------------------------------------------------------------------

def test_cancel_on_the_power_dialog_keeps_it_on(device):
    """Holding BOOT opens the power dialog; CANCEL closes it and nothing else
    happens (and it counts as a touch: the countdown starts again)."""
    paints = device.status()["paints"]
    device.button("boot", ms=2200)
    device.wait_status(lambda s: s["paints"] > paints, timeout=5)   # the sheet drawn
    time.sleep(0.5)
    device.tap(*CANCEL)
    time.sleep(1.5)
    assert device.running
    assert not device.status()["ride"]["recording"]
    assert "shutdown:" not in console_text(device)


def test_shut_down_mid_ride_keeps_the_ride_and_boots_clean(device):
    """SHUT DOWN with a ride recording finishes and keeps the ride: a valid
    FIT holding the ride's records, and the next boot has nothing to recover
    and no crash to report. It also leaves the last position for the next
    boot's GPS warm start."""
    device.gps_track(line_track(LAT, LON, 5), speed_kmh=30)
    device.wait_status(lambda s: s["ride"]["gps_fix"] and s["ride"]["time_valid"], timeout=15)
    device.set_speed(6)
    device.button("boot")
    device.wait_status(lambda s: s["ride"]["recording"], timeout=10)
    st = device.wait_status(lambda s: s["ride"]["elapsed_s"] >= 60, timeout=40)
    device.set_speed(1)
    device.button("boot", ms=2200)
    time.sleep(1.5)
    device.tap(*SHUT_DOWN)
    device.wait_powered_off(30)
    log = console_text(device)
    assert "shutdown: user power-off (dialog)" in log
    assert "ride saved:" in log

    name, data = only_ride(device)
    check_finished_fit(data)
    recs = fit_records(data)
    assert len(recs) >= 55, "%d records for a ride of %d s" % (len(recs), st["ride"]["elapsed_s"])
    last = recs[-1]
    assert abs(last["lat"] - LAT) < 0.01 and last["lon"] > LON

    device.power_on()
    assert device.status()["reset_reason"] == 8
    time.sleep(2)
    log = "\n".join(device.log())
    assert "ride interrupted" not in log and "ride recovered" not in log
    assert device.status()["screen"] == "dash"
    seed = device.wait_log(r"gps warm-start seed: ", since=0, timeout=5)
    lat, lon = (float(v) for v in seed.split("seed: ")[1].split(" ")[0].split(","))
    assert abs(lat - last["lat"]) < 0.001 and abs(lon - last["lon"]) < 0.001, seed
    assert open(os.path.join(device.sd, "rides", name), "rb").read() == data


# ---- the battery -------------------------------------------------------------------------------

def test_the_battery_level_and_charging_reach_the_app(device, link):
    """The app's battery readout is the fuel gauge's: the level as it falls,
    and the charging flag when the cable goes in."""
    link.expect_result(live_subscribe=pb.LiveSubscribe(topics=[pb.LIVE_TOPIC_RIDE]))
    device.battery(64)
    link.wait_push("live", lambda f: f.HasField("ride") and f.ride.battery_pct == 64 and not f.ride.charging,
                   timeout=10)
    device.battery(9)
    link.wait_push("live", lambda f: f.HasField("ride") and f.ride.battery_pct == 9, timeout=10)
    device.battery(9, charging=True)
    link.wait_push("live", lambda f: f.HasField("ride") and f.ride.charging and f.ride.battery_pct == 9,
                   timeout=10)
    assert device.status()["ride"]["battery"] == 9


def test_a_flat_battery_mid_ride_is_recovered_on_the_next_boot(device):
    """The firmware has no low-battery save: a battery that runs flat mid-ride
    browns the device out. The next boot must still offer the ride back, and
    SAVE must leave a valid FIT holding the ride up to its last checkpoint."""
    device.gps_track(line_track(LAT, LON, 5), speed_kmh=30)
    device.wait_status(lambda s: s["ride"]["gps_fix"] and s["ride"]["time_valid"], timeout=15)
    device.set_speed(6)
    device.button("boot")
    device.wait_status(lambda s: s["ride"]["recording"], timeout=10)
    device.battery(1)
    device.wait_status(lambda s: s["ride"]["elapsed_s"] >= 50, timeout=40)
    device.reset("brownout")
    device.wait_log(r"ride interrupted: /rides/", since=0, timeout=20)
    device.wait_status(lambda s: s["screen"] == "other", timeout=10)
    device.tap(*SAVE_BUTTON)
    device.wait_log(r"interrupted ride saved: .* \(ok\)", since=0, timeout=20)
    _, data = only_ride(device)
    check_finished_fit(data)
    assert len(fit_records(data)) >= 30      # every 15 s checkpointed; 50 s ridden
