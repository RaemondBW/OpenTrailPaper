"""Boot, the panel, and the GPS module's baud."""
import pytest

from otpsim import SimDevice


def test_boots_to_the_dashboard(device, tmp_path):
    st = device.wait_status(lambda s: s["screen"] == "dash" and s["paints"] > 0)
    assert st["page"] == 0
    assert st["ble"]["advertising"]
    assert not st["ble"]["bonds"]
    shot = device.screenshot(str(tmp_path / "dash.png"))
    assert open(shot, "rb").read(8) == b"\x89PNG\r\n\x1a\n"


@pytest.mark.parametrize("module_baud", [9600, 115200, 38400])
def test_gps_module_ends_at_115200_whatever_it_started_at(module_baud):
    """The L76K keeps its baud on its backup supply; the firmware finds it
    and moves it to 115200, and saves that (2026-10-25)."""
    with SimDevice.start(args=["--gps-baud", str(module_baud)]) as dev:
        st = dev.wait_status(lambda s: s["gps"]["module_baud"] == 115200 and s["gps"]["uart_baud"] == 115200,
                             timeout=20)
        assert st["gps"]["output"]
        line = dev.wait_log(r"gps module: CASIC", since=0, timeout=5)
        assert "CASIC" in line


def test_gps_fix_and_time(device):
    device.gps(37.7749, -122.4194, speed_kmh=0)
    st = device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    assert abs(st["ride"]["lat"] - 37.7749) < 1e-4
    assert abs(st["ride"]["lon"] + 122.4194) < 1e-4
    assert st["ride"]["time_valid"]
    # gps_service sets the clock at most once a minute, and not before the
    # first minute of uptime (lastClockSet starts at 0): run the clock fast.
    device.set_speed(10)
    st = device.wait_status(lambda s: s["wall_unix"] > 1_700_000_000, timeout=15)
    assert st["uptime_ms"] > 60000


@pytest.mark.xfail(reason="gps_service only sets the clock after 60 s of uptime "
                          "(lastClockSet starts at 0); a fix in the first minute leaves it unset",
                   strict=True)
def test_gps_sets_the_clock_on_the_first_fix(device):
    device.gps(37.7749, -122.4194)
    device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    device.wait_status(lambda s: s["wall_unix"] > 1_700_000_000, timeout=5)


def test_settings_survive_a_reboot(device):
    device.console("autopause 25")
    assert device.status()["settings"]["auto_pause_s"] == 25
    device.reboot()
    assert device.status()["settings"]["auto_pause_s"] == 25
