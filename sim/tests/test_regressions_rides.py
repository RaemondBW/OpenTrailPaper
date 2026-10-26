"""Regressions: ride recording, the SD card, boot and recovery.

One test per past field bug, each named after the commit that fixed it. Each
was checked against the unfixed behaviour (re-created by hand in today's
src/): it fails with the bug and passes with the fix.

`device.reboot()` is a hard reset: the sim re-execs with no shutdown path, so
whatever the firmware hadn't flushed to the card is lost — a power cut or a
watchdog reset mid-ride."""
import os
import re
import struct
import time

from otpsim import SimDevice
from otpsim.device import ControlError
from test_rides import SAVE_BUTTON, line_track, record_ride

FAST = ["--gps-baud", "115200"]


def unlocked_card_access(dev):
    return [l for l in dev.log() if "SD ACCESS WITHOUT sdLock" in l]


def lock_inversions(dev):
    return [l for l in dev.log() if "LOCK ORDER INVERSION" in l]


def start_riding(dev, km=5):
    dev.gps_track(line_track(37.77, -122.45, km), speed_kmh=30)
    dev.wait_status(lambda s: s["ride"]["gps_fix"] and s["ride"]["time_valid"], timeout=15)
    dev.set_speed(6)
    dev.button("boot")
    dev.wait_status(lambda s: s["ride"]["recording"], timeout=10)


def power_cut_mid_ride(dev, seconds=40):
    """Ride for `seconds`, then lose power. Returns the ride's file name."""
    rides = os.path.join(dev.sd, "rides")
    before = set(os.listdir(rides)) if os.path.isdir(rides) else set()
    start_riding(dev)
    dev.wait_status(lambda s: s["ride"]["elapsed_s"] >= seconds, timeout=seconds / 3 + 20)
    new = set(os.listdir(rides)) - before
    assert len(new) == 1, new
    dev.reboot()
    return new.pop()


def finished_fit(path, records=40):
    """A file shaped like a finished ride (header + data + CRC), which the
    recovery scan leaves alone."""
    data = bytes(100 + 25 * records)
    hdr = struct.pack("<BBHI4s", 12, 0x20, 2132, len(data), b".FIT")
    with open(path, "wb") as f:
        f.write(hdr + data + b"\0\0")


def fit_crc(data):
    table = [0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
             0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400]
    crc = 0
    for b in data:
        tmp = table[crc & 0xF]
        crc = (crc >> 4) & 0x0FFF
        crc = crc ^ tmp ^ table[b & 0xF]
        tmp = table[crc & 0xF]
        crc = (crc >> 4) & 0x0FFF
        crc = crc ^ tmp ^ table[(b >> 4) & 0xF]
    return crc


def log_line(dev, pattern, timeout):
    """The first console line matching `pattern` since boot, or None."""
    try:
        return dev.wait_log(pattern, timeout=timeout, since=0)
    except ControlError:
        return None


def console_lines(dev):
    with open(dev.log_path, "r", errors="replace") as f:
        return f.read().splitlines()


# ---- d456e93: recovery scanned the first 32 files, not the newest ------------------------

def test_an_interrupted_ride_on_a_full_card_is_still_found(device):
    """d456e93 (#65), "scan the newest 32 rides, not the first 32 in the
    directory". On a card with ~38 rides a mid-ride reset left today's ride
    unfinished and boot never noticed: no prompt, no repair, no log. The scan
    stopped after 32 directory entries, which FAT hands back oldest-first."""
    rides = os.path.join(device.sd, "rides")
    os.makedirs(rides, exist_ok=True)
    for i in range(38):
        finished_fit(os.path.join(rides, "20250101-08%02d00.fit" % i))
    cut = power_cut_mid_ride(device)
    assert cut not in ("20250101-08%02d00.fit" % i for i in range(38))
    line = log_line(device, r"ride interrupted: /rides/", timeout=20)
    assert line and "/rides/" + cut in line, "boot never noticed the interrupted ride %s" % cut
    device.wait_status(lambda s: s["screen"] == "other", timeout=10)   # the RIDE INTERRUPTED sheet


# ---- c075729: SD calls outside sdLock on the shared LoRa/SD bus ------------------------------

def test_boot_touches_the_card_only_under_the_sd_lock(device):
    """c075729, "Fix silent boot freeze with SD card inserted". The SD card
    shares its SPI bus with the LoRa radio; applySdUpdate()'s boot-time
    firmware.bin check ran SD.exists/rename/open with no sdLock, interleaved
    with a radio transfer and wedged the card — boot froze after "Maps ready".
    The sim has no radio to collide with, so it flags any card call made
    without the lock once tasks are running."""
    device.wait_log(r"boot: all tasks started", since=0)
    time.sleep(3)   # the UI task's start-up card work (recovery, sd update)
    assert unlocked_card_access(device) == []


def test_diag_flush_and_card_users_lock_in_one_order(device):
    """c075729, the ABBA deadlock in diag::flushToSD(): it took the diag mutex
    and then sdLock, while every task that logs under sdLock (ride recovery,
    tile saves, the recorder) takes them the other way round. The freeze needs
    an unlucky interleaving; the sim's lock-order check reports the two orders
    as soon as both have happened, hang or no hang."""
    record_ride(device, seconds=30)
    time.sleep(2)   # the BLE task's 1 Hz diag flush
    assert lock_inversions(device) == []


# ---- 19d8c48: the SD boot loop, the too-short mount, the wasted late card --------------------

def test_a_ride_file_that_kills_recovery_does_not_boot_loop():
    """19d8c48 (#31), "Break the SD boot loop". Recovery of interrupted rides
    once ran long enough to trip the interrupt watchdog; the reset tore one
    more file, so every boot died in the same place — a loop only pulling the
    card could break. The fix quarantines each file with a .bad marker before
    touching it, so the boot after a fatal file skips it.

    --sd-crash-on-read makes reading that file's contents reset the device,
    on every boot: the sim has no interrupt watchdog to trip, so the file is
    simply fatal."""
    dev = SimDevice(args=FAST + ["--sd-crash-on-read", "/rides/20250101-080000.fit"])
    os.makedirs(os.path.join(dev.sd, "rides"), exist_ok=True)
    finished_fit(os.path.join(dev.sd, "rides", "20250101-080000.fit"))
    try:
        dev.launch()
        deadline = time.time() + 30
        while time.time() < deadline:
            lines = console_lines(dev)
            if any("ride skipped (previous recovery did not survive it)" in l for l in lines):
                break
            time.sleep(0.2)
        resets = sum("[sim] reboot: watchdog" in l for l in console_lines(dev))
        time.sleep(5)   # and it stays up
        lines = console_lines(dev)
        assert sum("[sim] reboot: watchdog" in l for l in lines) == resets, "still boot-looping"
        assert any("ride skipped (previous recovery did not survive it): /rides/20250101-080000.fit" in l
                   for l in lines), "boot-looping on the same ride: %d resets" % resets
        assert resets == 1
    finally:
        dev.stop()


def test_a_slow_card_is_mounted_at_boot():
    """19d8c48 (#31), and cc7af12 before it ("SD not recognized"). Boot gave
    the card ~100 ms (three tries 50 ms apart); a known-good 32 GB SDHC only
    answered ~2 s after power-on, so boot always declared it absent: no
    recording, maps or routes until a later retry. --sd-ready-ms makes the
    sim's card fail SD.begin() until then."""
    with SimDevice.start(args=FAST + ["--sd-ready-ms", "1800"]) as dev:
        lines = console_lines(dev)
        boot = lines[:next(i for i, l in enumerate(lines) if "boot: all tasks started" in l)]
        assert not [l for l in boot if "boot mount failed" in l], "boot gave up on the card"
        assert [l for l in boot if "sd: ready" in l]


def test_a_card_that_mounts_late_is_put_to_use():
    """19d8c48 (#31), "stop a late card from being wasted". A card that only
    mounted on the 30 s retry, after boot gave up, stayed mounted and unused:
    boot had skipped everything that needs the card (routes, maps, the USB
    drive — and since d456e93 the interrupted-ride prompt) and nothing went
    back to it."""
    with SimDevice.start(args=FAST) as dev:
        cut = power_cut_mid_ride(dev, seconds=20)
        dev.wait_log(r"ride interrupted", timeout=20, since=0)
        dev.set_sd(False)
        dev.reboot()                         # boots with no card
        assert log_line(dev, r"boot mount failed", timeout=5)
        dev.set_speed(10)
        dev.set_sd(True)                     # the card comes back
        assert log_line(dev, r"sd: card back", timeout=15)
        line = log_line(dev, r"ride interrupted: /rides/", timeout=10)
        assert line and cut in line, "the late card's interrupted ride was never offered"
        dev.wait_status(lambda s: s["screen"] == "other", timeout=10)
        assert os.path.isdir(os.path.join(dev.sd, "routes"))


# ---- 0f7d05d: every ride's FIT CRC was 0x0000 ---------------------------------------------------

def test_a_saved_ride_carries_a_valid_fit_crc(device):
    """0f7d05d, "fix always-zero FIT CRC". Ride files were opened write-only
    ("w"); finish() computes the trailing CRC by reading the file back, which
    a write-only handle can't, so every ride ever written ended in 0x0000.
    Strava let it pass; strict FIT parsers rejected the rides outright."""
    record_ride(device, seconds=30)
    rides = os.listdir(os.path.join(device.sd, "rides"))
    assert len(rides) == 1, rides
    data = open(os.path.join(device.sd, "rides", rides[0]), "rb").read()
    assert data[8:12] == b".FIT"
    stored = struct.unpack("<H", data[-2:])[0]
    assert stored == fit_crc(data[:-2]), "file CRC 0x%04x, contents 0x%04x" % (stored, fit_crc(data[:-2]))


# ---- ba72100: Settings BACKLIGHT "-" bounced the rider out to the menu ----------------------

BACKLIGHT_ROW_Y = 64 + 2 * 108 + 54     # kMenuRowTop + kSettingsBacklightRow * kSettingsRowH
BACKLIGHT_MINUS = (192 + 44, BACKLIGHT_ROW_Y)   # kSettingsMinusX + kSettingsBtn / 2
BACKLIGHT_PLUS = (412 + 44, BACKLIGHT_ROW_Y)    # kSettingsPlusX + kSettingsBtn / 2


def test_settings_backlight_minus_dims_the_light(device):
    """ba72100, "Fix ... settings input". BACKLIGHT was drawn as a -/+
    stepper but hit-tested as a toggle row: only the toggle band (overlapping
    "+") did anything, and "-" fell through to "tap anywhere else", which
    bounced the rider out to the menu with the light unchanged."""
    device.console("screen settings")
    time.sleep(0.5)
    for _ in range(2):
        device.tap(*BACKLIGHT_PLUS)
        time.sleep(0.5)
    assert device.status()["settings"]["backlight"] == 2
    device.tap(*BACKLIGHT_MINUS)
    device.wait_status(lambda s: s["settings"]["backlight"] == 1, timeout=3)
    device.tap(*BACKLIGHT_MINUS)                  # still on Settings: dims again
    device.wait_status(lambda s: s["settings"]["backlight"] == 0, timeout=3)


# ---- 33ad1f3: the ride-history scan ran without the SD lock mid-ride -----------------------------

HISTORY_ROW = (270, 64 + 3 * 148 + 74)   # Menu row 3, RIDE HISTORY


def test_ride_history_mid_ride_scans_the_card_under_the_lock(device):
    """33ad1f3, "Fix ride history during recording: lock SD scan". The
    history screen walked /rides without sdLock while the recorder wrote FIT
    records on the same bus every second: the scan garbled that traffic and
    came back empty, so history looked inaccessible during a ride."""
    for i in range(3):
        finished_fit(os.path.join(device.sd, "rides", "20250101-08%02d00.fit" % i))
    start_riding(device)
    device.wait_status(lambda s: s["ride"]["elapsed_s"] >= 5, timeout=10)
    device.console("screen menu")
    time.sleep(0.5)
    device.tap(*HISTORY_ROW)
    time.sleep(2)
    assert device.status()["ride"]["recording"]
    assert unlocked_card_access(device) == []


# ---- d456e93: the shutdown tap changed nothing on the glass for seconds ---------------------

SHUT_DOWN = (24 + 246, 700 + 48)   # ui_render.cpp kPowerShutdown centre


def read_png(path):
    """The sim's own screenshots: 8-bit grey, one IDAT, filter 0 on every row."""
    import zlib
    data = open(path, "rb").read()
    w, h = struct.unpack(">II", data[16:24])
    pos, idat = 8, b""
    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        if data[pos + 4:pos + 8] == b"IDAT":
            idat += data[pos + 8:pos + 8 + n]
        pos += 12 + n
    raw = zlib.decompress(idat)
    return b"".join(raw[y * (w + 1) + 1:(y + 1) * (w + 1)] for y in range(h))


def test_the_shutdown_tap_is_acknowledged_before_the_ride_saves(device, tmp_path):
    """d456e93 (#65), "Acknowledge the shutdown tap before the slow part of
    shutdown". Confirming power-off ran seconds of card work (save the ride,
    flush the log) before anything changed on the glass, so the press read as
    a miss and got pressed again. A slow card (sd latency) holds the save
    open long enough to look at the panel while it runs."""
    start_riding(device)
    device.wait_status(lambda s: s["ride"]["elapsed_s"] >= 20, timeout=20)
    device.set_speed(1)
    device.button("boot", ms=2200)          # hold: the power dialog
    time.sleep(1.5)
    before = str(tmp_path / "dialog.png")
    device.screenshot(before)
    device.set_sd_latency(250)
    device.tap(*SHUT_DOWN)
    device.wait_log(r"shutdown: user power-off", timeout=10, since=0)
    during = str(tmp_path / "saving.png")
    device.screenshot(during)
    assert not [l for l in device.log() if "ride saved" in l], "the save finished too fast to look"
    a, b = read_png(before), read_png(during)
    changed = sum(x != y for x, y in zip(a, b)) / len(a)
    assert changed > 0.02, "the glass still shows the power dialog while the ride saves (%.2f%% changed)" % (
        changed * 100)
