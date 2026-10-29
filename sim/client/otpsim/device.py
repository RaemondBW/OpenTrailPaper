"""The firmware sim process and its control channel (sim/PROTOCOL.md)."""
import json
import os
import socket
import subprocess
import tempfile
import threading
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
BINARY = os.path.join(ROOT, "sim", "build", "otp-sim")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class ControlError(Exception):
    pass


class SimDevice:
    """One simulated OpenTrailPaper.

    `SimDevice.start()` launches sim/build/otp-sim on free ports with a fresh
    device directory (NVS, bonds, SD card), waits for it to boot and connects
    the control channel. Use it as a context manager, or call stop()."""

    def __init__(self, dir=None, ble_port=None, ctl_port=None, args=(), log_path=None):
        self.dir = dir or tempfile.mkdtemp(prefix="otp-sim-")
        self.ble_port = ble_port or free_port()
        self.ctl_port = ctl_port or free_port()
        self.args = list(args)
        self.log_path = log_path or os.path.join(self.dir, "console.log")
        self.proc = None
        self._sock = None
        self._file = None
        self._lock = threading.Lock()
        self._next_id = 1

    @property
    def sd(self):
        """The SD card's root on the host."""
        return os.path.join(self.dir, "sd")

    @classmethod
    def start(cls, boot_timeout=30, **kw):
        d = cls(**kw)
        d.launch(boot_timeout)
        return d

    def launch(self, boot_timeout=30):
        if not os.path.exists(BINARY):
            raise RuntimeError("build the sim first: sim/build.sh")
        log = open(self.log_path, "ab")
        self.proc = subprocess.Popen(
            [BINARY, "--dir", self.dir, "--ble-port", str(self.ble_port), "--ctl-port", str(self.ctl_port),
             "--no-stdin"] + self.args,
            stdout=log, stderr=subprocess.STDOUT)
        self._connect(boot_timeout)
        self.wait_log(r"boot: all tasks started", timeout=boot_timeout, since=0)

    def _connect(self, timeout):
        deadline = time.time() + timeout
        while True:
            try:
                self._sock = socket.create_connection(("127.0.0.1", self.ctl_port), timeout=60)
                self._file = self._sock.makefile("rw", encoding="utf-8")
                return
            except OSError:
                if self.proc and self.proc.poll() is not None:
                    raise RuntimeError("otp-sim exited during boot (see %s)" % self.log_path)
                if time.time() > deadline:
                    raise
                time.sleep(0.05)

    def reconnect_control(self, timeout=30):
        """After a reboot the sim re-execs: reattach to the new image."""
        try:
            self._sock.close()
        except Exception:
            pass
        self._connect(timeout)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            try:
                self.cmd("quit")
            except Exception:
                pass
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        if self._sock:
            self._sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()

    # -- control channel --------------------------------------------------------------------

    def cmd(self, cmd, **args):
        with self._lock:
            rid = self._next_id
            self._next_id += 1
            req = dict(args, cmd=cmd, id=rid)
            self._file.write(json.dumps(req) + "\n")
            self._file.flush()
            while True:
                line = self._file.readline()
                if not line:
                    raise ControlError("control channel closed")
                msg = json.loads(line)
                if msg.get("id") == rid:
                    break
        if not msg.get("ok"):
            raise ControlError("%s: %s" % (cmd, msg.get("error")))
        return msg

    def status(self):
        return self.cmd("status")["status"]

    def console(self, line, wait_ms=500):
        """A serial console command; returns the lines printed meanwhile."""
        return self.cmd("console", line=line, wait_ms=wait_ms)["lines"]

    def screenshot(self, path):
        return self.cmd("screenshot", path=path)["path"]

    def tap(self, x, y, ms=120):
        self.cmd("tap", x=x, y=y, ms=ms)

    def button(self, name="boot", ms=150):
        self.cmd("button", name=name, ms=ms)

    def gps(self, lat, lon, speed_kmh=0, course=0, alt=50, fix=True, sats=10, time=True):
        """What the module reports. With fix=False and time=False it is a cold
        receiver indoors: no position and no UTC either."""
        self.cmd("gps", lat=lat, lon=lon, speed_kmh=speed_kmh, course=course, alt=alt, fix=fix, sats=sats,
                 time=time)

    def gps_track(self, points, speed_kmh=25, loop=False):
        self.cmd("gps", track=[list(p) for p in points], speed_kmh=speed_kmh, loop=loop)

    def gps_silent(self, on=True):
        """The module stops sending (and hearing) anything: a loose connector
        or a brownout. Its state is kept; off again, it carries on."""
        self.cmd("gps_silent", on=on)

    def sensor(self, kind, **values):
        return self.cmd("sensor", kind=kind, **values)["addr"]

    def remove_sensor(self, addr):
        self.cmd("sensor_remove", addr=addr)

    def battery(self, pct, charging=False):
        self.cmd("battery", pct=pct, charging=charging)

    def set_wallclock(self, unix):
        self.cmd("wallclock", unix=unix)

    def set_sd(self, present):
        self.cmd("sd", present=present)

    def set_sd_latency(self, ms):
        """Every card open/read/write costs `ms` of device time (a slow card)."""
        self.cmd("sd", latency_ms=ms)

    def sd_usage(self):
        """The card's size and use, recounted from <dir>/sd: card_bytes,
        total_bytes, used_bytes, free_bytes, cluster_bytes."""
        return self.cmd("sd", usage=True)["sd"]

    def set_sd_size(self, mb, cluster_kb=0):
        """Swap in a card of `mb` MB with the same files on it."""
        return self.cmd("sd", size_mb=mb, cluster_kb=cluster_kb)["sd"]

    def fill_card(self, leave_bytes=0, name="filler.bin"):
        """Fill the card behind the firmware's back (a sparse file at its
        root) until only `leave_bytes` (rounded down to whole clusters) are
        free. Returns the usage afterwards."""
        u = self.sd_usage()
        cl = u["cluster_bytes"]
        path = os.path.join(self.sd, name)
        have = os.path.getsize(path) if os.path.exists(path) else 0
        have_alloc = -(-have // cl) * cl
        want = have_alloc + u["free_bytes"] - (leave_bytes // cl) * cl
        with open(path, "ab") as f:
            f.truncate(max(0, want))
        return self.sd_usage()

    def usb_host(self, on=True):
        """A computer mounts the card over USB (on) or ejects it (off). While
        it holds the card the firmware stays off it; a test edits `sd` in
        between, as the rider would in Finder. On release the device rescans
        its maps."""
        self.cmd("usb", host=on)

    def set_speed(self, factor):
        self.cmd("speed", factor=factor)

    def drop_phone(self, reason=0x08):
        """The radio link to the phone is lost (default: supervision timeout)."""
        self.cmd("ble_drop", reason=reason)

    def log_seq(self):
        return self.cmd("seq")["seq"]

    def log(self, since=0):
        return [l for _, l in self.cmd("log", since=since)["lines"]]

    def wait_log(self, pattern, timeout=10, since=None):
        """Wait for a console line matching `pattern` (a regex) after `since`
        (a log_seq(); default: from now). Returns the line."""
        args = dict(pattern=pattern, timeout_ms=int(timeout * 1000))
        if since is not None:
            args["since"] = since
        return self.cmd("wait_log", **args)["line"]

    def wait_status(self, predicate, timeout=10, interval=0.1):
        deadline = time.time() + timeout
        while True:
            st = self.status()
            if predicate(st):
                return st
            if time.time() > deadline:
                raise TimeoutError("status never matched; last: %s" % json.dumps(st)[:800])
            time.sleep(interval)

    # -- power (cov/workouts) ----------------------------------------------------------------

    def powered_off(self):
        """The device switched itself off (auto-sleep, SHUT DOWN): the sim
        process has exited."""
        return self.proc is not None and self.proc.poll() is not None

    def wait_powered_off(self, timeout=30):
        """Wait for the device to switch itself off. True if it did."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.powered_off():
                return True
            time.sleep(0.1)
        return self.powered_off()

    def workout(self):
        """The WORKOUT page's model (status "workout")."""
        return self.status()["workout"]

    def reboot(self, timeout=30):
        seq = 0
        self.cmd("reboot")
        time.sleep(0.3)
        self.reconnect_control(timeout)
        self.wait_log(r"boot: all tasks started", timeout=timeout, since=seq)
