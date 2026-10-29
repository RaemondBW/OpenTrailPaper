"""A long day out: hours of riding at a sped-up clock with the app's live
view connected, watching the firmware's memory. The sim's heap is the host's
malloc (esp_get_free_heap_size is a constant there), so a leak shows as the
process's resident memory growing with ride time. Slow: `sim/test.sh --slow`."""
import time

import pytest

from conftest import TIME_SCALE, paired_phone
from otpsim import LinkClient, pb
from power_util import check_finished_fit, fit_records, only_ride
from test_rides import SAVE_BUTTON, line_track

HOURS = 4
SPEED = 60


@pytest.mark.slow
def test_a_four_hour_ride_keeps_memory_flat_and_saves_whole(device):
    """Four hours at 25 km/h on an out-and-back road, the app's live view
    subscribed throughout. Resident memory after the first half hour (maps,
    caches, buffers warm) may not grow by more than a few MB to the end, and
    the ride saves as one valid FIT with a record for every second."""
    out = line_track(37.77, -122.45, 10)
    device.gps_track(out + out[::-1], speed_kmh=25, loop=True)
    device.wait_status(lambda s: s["ride"]["gps_fix"] and s["ride"]["time_valid"], timeout=15)
    phone = paired_phone(device)
    link = LinkClient(phone).hello()
    link.expect_result(live_subscribe=pb.LiveSubscribe(topics=[pb.LIVE_TOPIC_RIDE, pb.LIVE_TOPIC_POSITION]))
    device.button("boot")
    device.wait_status(lambda s: s["ride"]["recording"], timeout=10)
    device.set_speed(SPEED)

    def ride_to(seconds):
        return device.wait_status(lambda s: s["ride"]["elapsed_s"] >= seconds,
                                  timeout=(seconds / SPEED * 1.5 + 60) * TIME_SCALE, interval=1)

    ride_to(1800)
    warm = device.rss_kb()
    samples = []
    for h in range(1, HOURS + 1):
        ride_to(h * 3600)
        samples.append(device.rss_kb())
    grown_mb = (samples[-1] - warm) / 1024
    assert grown_mb < 8, "resident memory %d KB after 30 min, then %s KB hourly" % (warm, samples)
    assert device.status()["phone_connected"]

    device.set_speed(1)
    device.button("boot")
    device.wait_status(lambda s: s["screen"] == "other", timeout=10)
    device.tap(*SAVE_BUTTON)
    device.wait_status(lambda s: not s["ride"]["recording"], timeout=60)
    _, data = only_ride(device)
    check_finished_fit(data)
    recs = fit_records(data)
    assert len(recs) >= HOURS * 3600 - 5, len(recs)
    km = recs[-1]["distance_m"] / 1000
    assert 0.9 * 25 * HOURS < km < 1.05 * 25 * HOURS, km
