"""BLE sensors (src/ble_sensors.cpp, wheel_speed.h, speed_source.h): pairing
from the app and from the device's own PAIRED DEVICES screen, forgetting,
several sensors of one kind, reconnecting after a reboot, what each kind's
packets turn into on the dashboard and in the ride file, and sensors that go
quiet, go away, or come back with their counters reset.

The sensors are the sim's virtual peripherals (sim/PROTOCOL.md `sensor`):
standard Heart Rate, Cycling Power and CSC packets, so everything from the
scan to the decode is the firmware's own code."""
import time

import pytest

from conftest import TIME_SCALE, paired_phone
from otpsim import LinkClient, fit, pb
from sensorkit import (add_and_pair, eventually, glass, holds, links, listed, pair_sensor, save_ride,
                       scan_until_listed, sensor_list, settle, start_ride, state)

MENU_ROW_TOP, MENU_ROW_H = 64, 148           # ui_dashboard kMenuRowTop / kMenuRowH
SENSORS_MENU_ROW = (270, MENU_ROW_TOP + 2 * MENU_ROW_H + MENU_ROW_H // 2)


def row_centre(row):
    return (270, MENU_ROW_TOP + row * MENU_ROW_H + MENU_ROW_H // 2)


# ---- pairing, forgetting, several of a kind ----------------------------------------------------


def test_a_sensor_tapped_on_the_paired_devices_screen_pairs_and_connects(device, tmp_path):
    """The rider's own way in, no phone needed: MENU → SENSORS lists what the
    scan finds below the phone's row; tapping a strap pairs it, and the row
    turns inverted ("Connected") once the link is up."""
    addr = device.sensor("hr", bpm=133, name="HRM-Pro")
    device.console("screen menu")
    time.sleep(0.5)
    device.tap(*SENSORS_MENU_ROW)
    device.wait_status(lambda s: s["ble"]["scanning"], timeout=5 * TIME_SCALE)
    # The list is read at each 1 Hz render; wait for the strap's row to be drawn.
    y = row_centre(1)[1]
    eventually(lambda: glass(device, tmp_path, "list").ink(40, y - 50, 500, y + 50),
               lambda v: v > 0.02, "the strap never appeared on PAIRED DEVICES", 10)
    time.sleep(1.2)
    device.tap(*row_centre(1))
    eventually(lambda: state(device), lambda s: s["hr_bpm"] == 133, "the tapped strap never connected", 20)
    assert links(device)["HR"]["paired"] == addr
    # Connected rows are drawn inverted: the row band is mostly ink.
    settle(device)
    g = glass(device, tmp_path, "connected")
    assert g.ink(40, y - 30, 500, y + 30) > 0.5, "the connected strap's row isn't inverted"


def test_the_app_sees_the_sensor_by_its_make_once_connected(device, link):
    """A strap advertising a bare name is listed by it while scanning; once it
    has connected, the list carries the Device Information make ("Garmin
    HRM-Dual") the rider recognises, with the right kind bits, paired and
    connected."""
    addr = device.sensor("hr", bpm=120, name="HRM9481", make="Garmin", model="HRM-Dual")
    l = scan_until_listed(link, addr)
    row = [s for s in l.sensors if s.address == addr][0]
    assert row.name == "HRM9481" and row.kinds == 1 and not row.paired, row
    pair_sensor(link, addr)
    eventually(lambda: listed(link, addr), lambda r: r and r.connected,
               "the sensor list never showed the strap connected", 20)
    row = listed(link, addr)
    assert row.name == "Garmin HRM-Dual" and row.paired and row.kinds == 1, row


def test_an_unidentified_speed_cadence_sensor_is_offered_as_both(device, link):
    """0x1816 alone can't say whether a sensor measures speed, cadence or
    both, so a fresh one is listed with both bits (b2 cadence, b4 speed)."""
    addr = device.sensor("speed", speed_kmh=20)
    l = scan_until_listed(link, addr)
    row = [s for s in l.sensors if s.address == addr][0]
    assert row.kinds == (1 << 2) | (1 << 4), row


def test_forgetting_a_sensor_from_the_app_drops_it_for_good(device, link):
    """FORGET disconnects the strap and clears its saved address: it is still
    in range and advertising, but the device must never connect to it again
    — the saved MAC is all that keeps a stranger's strap off the dashboard."""
    addr = add_and_pair(device, link, "hr", bpm=141)
    eventually(lambda: state(device), lambda s: s["hr_bpm"] == 141, "the strap never connected", 20)
    r = link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_FORGET, address=addr))
    assert not any(s.address == addr and s.paired for s in r.sensor_list.sensors), r
    eventually(lambda: state(device), lambda s: not s["hr_connected"], "the forgotten strap stayed connected", 10)
    assert links(device)["HR"]["paired_all"] == []
    # Keep the hunt going (the app's scan) for a while: it must stay off.
    link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_SCAN_START))
    holds(lambda: state(device), lambda s: not s["hr_connected"] and s["hr_bpm"] == -1,
          "the device reconnected to a forgotten strap", 6)
    link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_SCAN_STOP))


@pytest.mark.xfail(strict=True, reason=(
    "BUG: onSensors FORGET (src/link.cpp) answers with sendSensorList right after ble_sensors::forget "
    "has only *asked* for the disconnect; Sensor::connected is cleared later in the client's "
    "onDisconnect, so the answer lists the forgotten strap as connected — and with the scan "
    "stopped no further list comes, so the app's Sensors sheet shows it 'Connected' until reopened"))
def test_the_answer_to_forget_no_longer_lists_the_strap_as_connected(device, link):
    addr = add_and_pair(device, link, "hr", bpm=141)
    eventually(lambda: listed(link, addr), lambda r: r and r.connected, "the strap never connected", 20)
    r = link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_FORGET, address=addr))
    row = [x for x in r.sensor_list.sensors if x.address == addr]
    assert not (row and row[0].connected), "the FORGET answer still shows the strap connected: %s" % row


def test_pairing_without_an_address_is_a_bad_request(link):
    r = link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_PAIR, address=""))
    assert r.WhichOneof("body") == "result" and r.result.code == pb.RESULT_BAD_REQUEST, r


@pytest.mark.xfail(strict=True, reason=(
    "BUG: pairCandidate (src/ble_sensors.cpp) disconnects the live strap to switch to the newly "
    "paired one, but the scan callback takes the first paired address it hears — the old strap, "
    "still in range — so the rider who pairs a new strap keeps getting the old one's heart rate"))
def test_a_newly_paired_strap_takes_over_from_the_one_in_range(device, link):
    """pairCandidate: 'Switch to the newly paired device now if a different
    one is up.' The old strap stays paired (pairing ADDS: the trainer strap
    must not forget the road one), but the new one is what the rider just
    asked for."""
    a = add_and_pair(device, link, "hr", bpm=120, addr="c0:ff:ee:00:01:01", name="Strap A")
    eventually(lambda: state(device), lambda s: s["hr_bpm"] == 120, "strap A never connected", 20)
    b = add_and_pair(device, link, "hr", bpm=150, addr="c0:ff:ee:00:01:02", name="Strap B")
    eventually(lambda: links(device)["HR"], lambda l: l["live"] == b,
               "pairing strap B reconnected strap A instead", 15)


def test_two_straps_stay_paired_and_whichever_is_in_range_connects(device, link):
    """Several sensors of one kind: a road strap and a trainer strap are both
    remembered. With only one in range, that one connects — and when it goes
    and the other turns up, the device switches without being asked."""
    road = add_and_pair(device, link, "hr", bpm=120, addr="c0:ff:ee:00:02:01", name="Road")
    eventually(lambda: state(device), lambda s: s["hr_bpm"] == 120, "the road strap never connected", 20)
    device.remove_sensor(road)                       # left at home
    eventually(lambda: state(device), lambda s: not s["hr_connected"], "the road strap never dropped", 10)
    trainer = add_and_pair(device, link, "hr", bpm=95, addr="c0:ff:ee:00:02:02", name="Trainer")
    eventually(lambda: state(device), lambda s: s["hr_bpm"] == 95, "the trainer strap never connected", 20)
    assert set(links(device)["HR"]["paired_all"]) == {road, trainer}
    # Next day on the road: the trainer strap is out of range, the road one back.
    device.remove_sensor(trainer)
    device.sensor("hr", bpm=131, addr=road, name="Road")
    eventually(lambda: state(device), lambda s: s["hr_bpm"] == 131,
               "the remembered road strap never reconnected", 30)
    assert links(device)["HR"]["live"] == road


def test_paired_sensors_reconnect_after_a_reboot(device, link):
    """Pairings live in NVS: after a restart the first-90-seconds hunt finds
    the strap and the power meter again without the phone, and the Sensors
    list shows them by name before they're even connected."""
    hr = add_and_pair(device, link, "hr", bpm=128, make="Polar", model="H10")
    pw = add_and_pair(device, link, "power", watts=180, cadence=80, make="Favero", model="Assioma")
    eventually(lambda: state(device), lambda s: s["hr_bpm"] == 128 and s["power_w"] == 180,
               "the sensors never connected", 25)
    device.reboot()
    # The virtual sensors live in the sim process, which a reboot re-execs:
    # put the same two back in the world. The strap is asleep for now.
    device.sensor("hr", addr=hr, bpm=128, make="Polar", model="H10", advertising=False)
    device.sensor("power", addr=pw, watts=180, cadence=80, make="Favero", model="Assioma")
    l = links(device)
    assert l["HR"]["paired"] == hr and l["Power"]["paired"] == pw
    assert l["HR"]["name"] == "Polar H10" and not l["HR"]["connected"], l["HR"]
    eventually(lambda: state(device), lambda s: s["power_w"] == 180, "the power meter never came back", 25)
    device.sensor("hr", addr=hr, advertising=True)       # strap woken by a heartbeat
    eventually(lambda: state(device), lambda s: s["hr_bpm"] == 128, "the strap never came back", 25)


# ---- what the packets turn into ----------------------------------------------------------------


def test_heart_rate_in_the_16_bit_format_and_above_254_is_clamped(device, link):
    """Flags bit 0 selects a 16-bit value. 254 is the most a FIT uint8 can say
    that isn't 'invalid' (0xFF), so anything above is held at 254."""
    addr = add_and_pair(device, link, "hr", bpm=181, hr_wide=True, contact=True)
    eventually(lambda: state(device), lambda s: s["hr_bpm"] == 181, "a 16-bit heart rate was misread", 20)
    device.sensor("hr", addr=addr, bpm=300)
    eventually(lambda: state(device), lambda s: s["hr_bpm"] == 254, "300 bpm wasn't clamped to 254", 10)


def test_a_power_meter_with_balance_and_torque_fields_still_gives_cadence(device, link):
    """Cycling Power's crank data sits after every optional field the flags
    announce — pedal balance (1 byte), accumulated torque (2) and wheel
    revolutions (6). Walk one wrong and cadence is read out of the wrong
    bytes."""
    add_and_pair(device, link, "power", watts=245, cadence=92, balance=52, torque=True, power_wheel=True,
                 speed_kmh=32)
    s = eventually(lambda: state(device), lambda s: s["power_w"] == 245 and s["cadence_rpm"] > 0,
                   "the power meter's values never arrived", 20)
    time.sleep(3)
    s = state(device)
    assert s["power_w"] == 245
    assert 88 <= s["cadence_rpm"] <= 96, "cadence read from the wrong bytes: %s" % s["cadence_rpm"]


def test_the_3s_power_average_follows_the_rider_and_decays(device, link):
    """The hero's POWER · 3S is the mean of the readings in the last three
    seconds — recomputed every second even between packets, so easing off
    shows within three seconds rather than lingering."""
    pw = add_and_pair(device, link, "power", watts=300, cadence=90)
    eventually(lambda: state(device), lambda s: s["power3s_w"] == 300, "3 s power never settled at 300", 20)
    device.sensor("power", addr=pw, watts=100)
    s = eventually(lambda: state(device), lambda s: s["power_w"] == 100, "instant power never followed", 5)
    assert s["power3s_w"] > 100, "the 3 s average jumped straight to the new reading: %s" % s
    eventually(lambda: state(device), lambda s: s["power3s_w"] == 100,
               "the 3 s average still held the old effort", 4.5)
    device.sensor("power", addr=pw, watts=0, cadence=0)
    eventually(lambda: state(device), lambda s: s["power3s_w"] == 0, "the 3 s average never decayed to 0", 4.5)


@pytest.mark.xfail(strict=True, reason=(
    "BUG: markDisconnected (src/ble_sensors.cpp) clears power for KIND_POWER but not the cadence "
    "onPowerNotify derives from the meter's crank data, so a dropped power meter leaves its last "
    "cadence live on the dashboard, the app and every FIT record for 15 s (until the staleness sweep)"))
def test_a_power_meter_that_drops_out_takes_its_cadence_with_it(device, link):
    """A power meter with crank data is the cadence sensor too (bdbec8d).
    When its link drops the POWER field greys at once; CADENCE, from the
    very same meter, must go with it."""
    pw = add_and_pair(device, link, "power", watts=210, cadence=90)
    eventually(lambda: state(device), lambda s: s["power_w"] == 210 and 85 <= s["cadence_rpm"] <= 95,
               "the meter's power and cadence never arrived", 20)
    device.remove_sensor(pw)
    eventually(lambda: state(device), lambda s: not s["power_connected"], "the dropped meter stayed connected", 10)
    s = eventually(lambda: state(device), lambda s: s["cadence_rpm"] == -1 or not s["cadence_connected"],
                   "cadence from the vanished power meter is still shown", 3)


@pytest.mark.xfail(strict=True, reason=(
    "BUG: the power meter's crank state (crankFromPower in src/ble_sensors.cpp) is never reset on "
    "reconnect, unlike the CSC links' (connectSensor resets crankFromCsc), so a meter that comes "
    "back with its counters restarted (battery change, reboot) shows a 254 rpm cadence spike that "
    "is also written to the ride file"))
def test_a_power_meter_back_with_reset_counters_shows_no_cadence_spike(device, link):
    pw = add_and_pair(device, link, "power", watts=200, cadence=85)
    eventually(lambda: state(device), lambda s: 80 <= s["cadence_rpm"] <= 90, "no cadence from the meter", 20)
    time.sleep(3)                                     # well into its crank counter
    device.remove_sensor(pw)
    eventually(lambda: state(device), lambda s: not s["power_connected"], "the meter never dropped", 10)
    # The rider swaps the battery: the meter restarts counting from zero.
    device.sensor("power", addr=pw, watts=200, cadence=85, reset=True)
    worst = 0
    deadline = time.time() + 25 * TIME_SCALE
    seen = 0
    while time.time() < deadline and seen < 6:
        s = state(device)
        if s["power_connected"] and s["cadence_rpm"] >= 0:
            worst = max(worst, s["cadence_rpm"])
            seen += 1
        time.sleep(0.25)
    assert seen, "the meter never came back"
    assert worst <= 100, "cadence spiked to %d rpm after the meter came back" % worst


# ---- speed and cadence sensors -----------------------------------------------------------------


def test_a_speed_only_sensor_moves_itself_to_the_speed_slot(device, link):
    """Paired into Cadence (all an advert can say), a sensor whose CSC Feature
    says 'wheel only' re-files itself as the Speed sensor and reconnects
    there: speed comes from the wheel, and cadence isn't claimed."""
    addr = add_and_pair(device, link, "speed", speed_kmh=27)
    s = eventually(lambda: state(device), lambda s: s["speed_source"] == "sensor" and s["speed_kmh"] > 20,
                   "the speed sensor never became the speed source", 25)
    l = links(device)
    assert l["Speed"]["paired"] == addr and l["Speed"]["connected"], l["Speed"]
    assert l["Cadence"]["paired_all"] == [], l["Cadence"]
    assert abs(s["speed_kmh"] - 27) < 1.0, s
    assert not s["cadence_connected"], "a wheel-only sensor is claiming cadence"
    row = listed(link, addr)
    assert row.kinds == 1 << 4, row     # Speed, now that it is known


def test_separate_speed_and_cadence_sensors_both_connect(device, link):
    """Two sensors behind the same 0x1816 service — one on the wheel, one on
    the crank — each get a link of their own and feed their own field."""
    add_and_pair(device, link, "speed", speed_kmh=31)
    eventually(lambda: links(device)["Speed"], lambda l: l["connected"], "the speed sensor never connected", 25)
    add_and_pair(device, link, "cadence", cadence=78)
    s = eventually(lambda: state(device), lambda s: 74 <= s["cadence_rpm"] <= 82 and abs(s["speed_kmh"] - 31) < 1,
                   "speed and cadence never both arrived", 25)
    l = links(device)
    assert l["Speed"]["connected"] and l["Cadence"]["connected"], l
    assert s["speed_source"] == "sensor"


def test_a_csc_sensor_without_its_feature_characteristic_learns_from_packets(device, link):
    """The CSC Feature characteristic is mandatory, but not every sensor gets
    it right. Without it the firmware learns what the sensor measures from
    the measurement flags — a combo gives speed and cadence."""
    add_and_pair(device, link, "csc", speed_kmh=24, cadence=88, feature=None)
    eventually(lambda: state(device),
               lambda s: s["speed_source"] == "sensor" and abs(s["speed_kmh"] - 24) < 1 and 84 <= s["cadence_rpm"] <= 92,
               "a combo sensor without a Feature characteristic gave no speed or cadence", 25)
    assert links(device)["Cadence"]["connected"]


def test_wheel_speed_follows_the_wheel_size_setting(device, link):
    """Speed is revolutions x circumference: a rider who sets a bigger tyre
    (2300 mm) sees the same revolutions read 2300/2105 faster."""
    add_and_pair(device, link, "speed", speed_kmh=30, wheel_mm=2105)
    eventually(lambda: state(device), lambda s: abs(s["speed_kmh"] - 30) < 0.8,
               "the wheel never read 30 km/h at the default circumference", 25)
    r = link.request(set_settings=pb.Settings(wheel_mm=2300))
    assert r.settings.wheel_mm == 2300, r
    want = 30 * 2300 / 2105.0
    eventually(lambda: state(device), lambda s: abs(s["speed_kmh"] - want) < 0.8,
               "speed didn't follow the new wheel size (want %.1f)" % want, 10)


def test_the_wheel_revolution_counter_rolls_over_seamlessly(device, link):
    """The cumulative wheel count is a u32; crossing 2^32 is one more
    revolution, not a counter that went backwards: speed holds and distance
    keeps counting, with no glitch reseed."""
    since = device.log_seq()
    addr = add_and_pair(device, link, "speed", speed_kmh=36, wheel_revs=2 ** 32 - 40)
    s0 = eventually(lambda: state(device), lambda s: abs(s["speed_kmh"] - 36) < 1,
                    "the wheel never read 36 km/h", 25)
    t0, d0 = time.time(), s0["wheel_dist_m"]
    holds(lambda: state(device), lambda s: abs(s["speed_kmh"] - 36) < 1.5,
          "speed faltered as the counter rolled over", 8)
    s1 = state(device)
    rate = (s1["wheel_dist_m"] - d0) / (time.time() - t0)
    assert 7 < rate < 13, "distance grew at %.1f m/s over the rollover (36 km/h is 10)" % rate
    assert not [l for l in device.log(since) if "glitch" in l], "the rollover was treated as a glitch"


def test_a_wheel_counter_that_restarts_is_reseeded_not_counted(device, link):
    """A sensor reset (battery swap) sends its counter back to zero. That is a
    glitch to reseed from, never 4 billion revolutions of distance: the
    distance total must not jump, and speed returns."""
    addr = add_and_pair(device, link, "speed", speed_kmh=25, wheel_revs=500000)
    s0 = eventually(lambda: state(device), lambda s: abs(s["speed_kmh"] - 25) < 1, "no wheel speed", 25)
    since = device.log_seq()
    device.sensor("speed", addr=addr, reset=True)
    device.wait_log(r"wheel counter glitch", timeout=10 * TIME_SCALE, since=since)
    s1 = state(device)
    assert s1["wheel_dist_m"] - s0["wheel_dist_m"] < 100, "distance jumped on a counter reset: %s" % s1
    eventually(lambda: state(device), lambda s: abs(s["speed_kmh"] - 25) < 1, "speed never came back", 10)


def test_wheel_speed_wins_over_gps_and_hands_back_when_the_sensor_goes(device, link):
    """speed_source: a talking wheel sensor beats GPS (it answers within a
    revolution); when it goes out of range the speed falls straight back to
    GPS on the next pass."""
    device.gps(37.77, -122.45, speed_kmh=18, course=90)
    device.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
    addr = add_and_pair(device, link, "speed", speed_kmh=29)
    eventually(lambda: state(device), lambda s: s["speed_source"] == "sensor" and abs(s["speed_kmh"] - 29) < 1,
               "the wheel sensor never took over from GPS", 25)
    frame = link.wait_push("live", lambda f: f.ride.speed_from_wheel, timeout=5)
    assert abs(frame.ride.speed_dkmh / 10.0 - 29) < 1.5
    device.remove_sensor(addr)
    eventually(lambda: state(device), lambda s: s["speed_source"] == "gps" and abs(s["speed_kmh"] - 18) < 1,
               "speed never fell back to GPS", 10)


def test_a_stopped_wheel_contradicted_by_gps_yields_to_gps(device, link):
    """A wheel sensor that says 'stopped' while a good fix says 25 km/h has
    lost its magnet or sits on the wrong bike: after 10 s, GPS wins."""
    addr = add_and_pair(device, link, "speed", speed_kmh=0)
    eventually(lambda: links(device)["Speed"], lambda l: l["connected"], "the speed sensor never connected", 25)
    device.gps(37.77, -122.45, speed_kmh=25, course=90)
    eventually(lambda: state(device), lambda s: s["speed_source"] == "gps" and abs(s["speed_kmh"] - 25) < 1,
               "a still wheel kept the speed at 0 against a moving fix", 20)


# ---- dropouts on the dashboard -----------------------------------------------------------------

HERO_BODY = (40, 130, 500, 300)     # inside the default layout's POWER hero, above the value


def test_a_greyed_power_hero_comes_back_when_the_meter_does(device, link, tmp_path):
    """The other half of 39bd282: a meter that goes quiet greys the hero; one
    that starts talking again must un-grey it, not leave a live number under
    the 'gone' tone."""
    pw = add_and_pair(device, link, "power", watts=230, cadence=88)
    eventually(lambda: state(device), lambda s: s["power_w"] == 230, "the meter never connected", 20)
    device.set_speed(5)
    device.sensor("power", addr=pw, sending=False)
    device.wait_status(lambda s: s["ride"]["power"] == -1, timeout=40)
    device.set_speed(1)
    settle(device)
    assert glass(device, tmp_path, "grey").dots(*HERO_BODY) > 0.6, "the silent meter's hero isn't greyed"
    device.sensor("power", addr=pw, sending=True, watts=240)
    device.wait_status(lambda s: s["ride"]["power"] == 240, timeout=15 * TIME_SCALE)
    settle(device)
    tone = glass(device, tmp_path, "back").dots(*HERO_BODY)
    assert tone < 0.2, "the hero stayed greyed with the meter back (%.2f)" % tone


def test_a_silent_strap_drops_its_reading_from_the_app_after_15_s(device, link):
    """A strap that's connected but no longer sending (off the chest) must not
    leave its last heart rate in the live stream; when it starts again the
    reading returns without a reconnect."""
    hr = add_and_pair(device, link, "hr", bpm=150)
    link.wait_push("live", lambda f: f.ride.HasField("hr_bpm") and f.ride.hr_bpm == 150, timeout=20 * TIME_SCALE)
    device.set_speed(4)
    device.sensor("hr", addr=hr, sending=False)
    link.wait_push("live", lambda f: not f.ride.HasField("hr_bpm"), timeout=15 * TIME_SCALE)
    device.set_speed(1)
    assert state(device)["hr_connected"], "a silent strap was disconnected, not just blanked"
    device.sensor("hr", addr=hr, sending=True, bpm=152)
    link.wait_push("live", lambda f: f.ride.HasField("hr_bpm") and f.ride.hr_bpm == 152, timeout=10 * TIME_SCALE)


# ---- into the ride file ------------------------------------------------------------------------


def test_the_ride_file_carries_sensor_values_and_drops_stale_ones(device, link):
    """FIT records take HR, power and cadence from the sensors each second.
    A strap that goes quiet mid-ride must turn into 'no value' in the
    records (and out of the average), not its last beat repeated; the
    session's averages are the records' averages."""
    hr = add_and_pair(device, link, "hr", bpm=140)
    pw = add_and_pair(device, link, "power", watts=220, cadence=90)
    eventually(lambda: state(device), lambda s: s["hr_bpm"] == 140 and s["power_w"] == 220 and s["cadence_rpm"] > 80,
               "sensors never connected", 25)
    start_ride(device)
    device.set_speed(4)
    device.wait_status(lambda s: s["ride"]["elapsed_s"] >= 20, timeout=20 * TIME_SCALE)
    device.sensor("hr", addr=hr, sending=False)
    device.wait_status(lambda s: s["ride"]["elapsed_s"] >= 55, timeout=20 * TIME_SCALE)
    device.sensor("hr", addr=hr, sending=True, bpm=160)
    device.sensor("power", addr=pw, watts=260)
    device.wait_status(lambda s: s["ride"]["elapsed_s"] >= 75, timeout=20 * TIME_SCALE)
    device.set_speed(1)
    data = save_ride(device)
    recs = fit.records(data)
    assert len(recs) >= 70, len(recs)
    hrs = [r["hr_bpm"] for r in recs]
    assert hrs.count(140) >= 20, hrs
    gap = [i for i, v in enumerate(hrs) if v is None and i > 20]
    assert len(gap) >= 10, "the silent strap's last beat was recorded through the gap: %s" % hrs
    assert 160 in hrs[gap[-1]:], "the strap's return never reached the file: %s" % hrs
    assert set(v for v in hrs if v is not None) <= {140, 160}, hrs
    pows = [r["power_w"] for r in recs if r["power_w"] is not None]
    assert set(pows) <= {220, 260} and pows.count(260) >= 5, pows
    cads = [r["cadence_rpm"] for r in recs if r["cadence_rpm"] is not None]
    assert cads and all(80 <= c <= 100 for c in cads[3:]), cads
    ses = fit.session(data)
    valid = [v for v in hrs if v is not None]
    assert abs(ses["avg_hr_bpm"] - sum(valid) / len(valid)) <= 1, (ses, valid)
    assert abs(ses["avg_power_w"] - sum(pows) / len(pows)) <= 1, (ses, pows)


def test_an_indoor_ride_on_a_wheel_sensor_records_distance_and_speed(device, link):
    """A trainer ride: no fix ever, but a wheel sensor. The records are
    position-less and still carry the wheel's speed and a growing distance,
    and the session's distance is the wheel's."""
    add_and_pair(device, link, "speed", speed_kmh=30)
    eventually(lambda: state(device), lambda s: abs(s["speed_kmh"] - 30) < 1, "no wheel speed", 25)
    start_ride(device, gps=False)
    device.set_speed(4)
    device.wait_status(lambda s: s["ride"]["elapsed_s"] >= 60, timeout=30 * TIME_SCALE)
    device.set_speed(1)
    data = save_ride(device)
    recs = fit.records(data)
    assert len(recs) >= 50, "an indoor ride wrote %d records" % len(recs)
    assert all(r["lat"] is None for r in recs), "records carry a position with no fix"
    speeds = [r["speed_ms"] * 3.6 for r in recs[3:] if r["speed_ms"] is not None]
    assert speeds and all(abs(v - 30) < 2 for v in speeds), speeds
    dists = [r["distance_m"] for r in recs]
    assert all(b >= a for a, b in zip(dists, dists[1:])), "distance went backwards"
    per_s = (dists[-1] - dists[5]) / max(1, len(dists) - 6)
    assert 7.3 < per_s < 9.4, "distance grew %.1f m per record at 30 km/h (8.3)" % per_s
    ses = fit.session(data)
    assert abs(ses["total_distance_m"] - dists[-1]) < 20, (ses, dists[-1])
