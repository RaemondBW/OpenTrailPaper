#!/usr/bin/env python3
"""Screen tour: boot a sim device, put it through the states a rider sees,
and save the panel at each one, for reviewing UI changes without hardware.

    sim/.venv/bin/python sim/tour.py [outdir]     (default sim/build/tour)

Writes one PNG per state and an index.html showing them side by side."""
import math
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "client"))

from otpsim import LinkClient, SimDevice, SimPhone, pb  # noqa: E402


def track(lat, lon, km, step_m=40):
    return [(lat, lon + i * step_m / (111320.0 * math.cos(math.radians(lat))))
            for i in range(int(km * 1000 / step_m) + 1)]


def main():
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else
                          os.path.join(os.path.dirname(__file__), "build", "tour"))
    os.makedirs(out, exist_ok=True)
    shots = []

    with SimDevice.start(args=["--gps-baud", "115200"]) as dev:
        def shot(name, settle=1.2):
            time.sleep(settle)
            path = os.path.join(out, name + ".png")
            dev.screenshot(path)
            shots.append(name)
            print("  ", name)

        shot("boot-no-phone")
        phone = SimPhone(dev.ble_port).connect()
        phone.wait_security(10)
        link = LinkClient(phone).hello()
        dev.gps(37.7749, -122.4194, sats=12)
        dev.wait_status(lambda s: s["ride"]["gps_fix"], timeout=10)
        shot("dash-phone-connected")

        hr = dev.sensor("hr", bpm=148)
        pw = dev.sensor("power", watts=231, cadence=88)
        link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_SCAN_START))
        for a in (hr, pw):
            link.wait_push("sensor_list", lambda l, a=a: any(s.address == a for s in l.sensors), timeout=10)
            link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_PAIR, address=a))
        link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_SCAN_STOP))
        dev.wait_status(lambda s: s["ride"]["hr"] == 148 and s["ride"]["power"] == 231, timeout=30)
        shot("dash-sensors-live")

        dev.gps_track(track(37.7749, -122.4194, 5), speed_kmh=28)
        dev.button("boot")
        dev.wait_status(lambda s: s["ride"]["recording"], timeout=10)
        dev.set_speed(5)
        dev.wait_status(lambda s: s["ride"]["elapsed_s"] > 120, timeout=60)
        dev.set_speed(1)
        shot("dash-recording")

        link.tell(show_page=pb.ShowPage(page=1))
        shot("page-2", 2)
        for name in ("map", "menu", "settings", "gps", "layers"):
            dev.console("screen " + name)
            shot("screen-" + name, 1.5)
        dev.console("screen dash")

        dev.button("boot")
        shot("ride-summary", 1.5)
        dev.tap(144, 748)   # Save
        dev.wait_status(lambda s: not s["ride"]["recording"], timeout=20)
        shot("after-save")

        dev.cmd("sensor", kind="power", addr=pw, sending=False)
        dev.wait_status(lambda s: s["ride"]["power"] == -1, timeout=40)
        shot("dash-power-dropped")

    with open(os.path.join(out, "index.html"), "w") as f:
        f.write("<!doctype html><meta charset=utf-8><title>Panel tour</title>"
                "<style>body{font:14px system-ui;background:#eee;margin:16px}"
                "figure{display:inline-block;margin:8px;background:#fff;padding:8px}"
                "img{width:270px;border:1px solid #999;display:block}</style>")
        for s in shots:
            f.write('<figure><img src="%s.png"><figcaption>%s</figcaption></figure>' % (s, s))
    print("wrote", out)


if __name__ == "__main__":
    main()
