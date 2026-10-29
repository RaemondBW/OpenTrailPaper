"""Shared helpers for the mesh tests: a device with its LoRa radio fitted and
switched on, the phone's view of it, and a stock Meshtastic node's view of
what it puts on the air (otpsim.mesh — an independent codec)."""
import base64
import os
import shutil
import time

import pytest

from conftest import ARTIFACTS, TIME_SCALE, paired_phone
from otpsim import LinkClient, SimDevice, pb
from otpsim import mesh as M

BROADCAST = M.BROADCAST


def start_device(mac=None, args=()):
    a = ["--gps-baud", "115200", "--lora"] + list(args)
    if mac:
        a += ["--mac", mac]
    return SimDevice.start(args=a)


def node_num_for(mac):
    b = bytes(int(x, 16) for x in mac.split(":"))
    return int.from_bytes(b[2:6], "little")


class Mesh:
    """One device with its radio on, a paired phone on the link, and helpers
    that read the mesh the way the apps do (MeshCommand get, every page)."""

    def __init__(self, dev, radio_on=True):
        self.dev = dev
        self.phone = paired_phone(dev)
        self.link = LinkClient(self.phone).hello()
        self.num = self.state().node_num
        line = dev.wait_log(r"PKI key pair, public key", since=0, timeout=10)
        self.public_key = base64.b64decode(line.split("public key ")[1].strip())
        self.tx_seq = 0
        # Hears what the device sends, on the default channel, as a stock node.
        self.ear = M.VirtualNode(0x0EA20001, "Ear", "EAR")
        self.channels = [M.Channel()]
        if radio_on:
            self.radio(True)

    # -- the phone's view -------------------------------------------------------------------

    def get(self, topic):
        r = self.link.request(mesh=pb.MeshCommand(get=pb.MeshGet(topics=[topic])), all_pages=True)
        return r

    def state(self):
        return self.get(pb.MESH_TOPIC_STATE)[0].mesh_state

    def stats(self):
        return self.get(pb.MESH_TOPIC_STATS)[0].mesh_stats

    def nodes(self):
        return [n for m in self.get(pb.MESH_TOPIC_NODES) for n in m.mesh_nodes.nodes]

    def history(self):
        return [x for m in self.get(pb.MESH_TOPIC_HISTORY) for x in m.mesh_history.messages]

    def device_channels(self):
        return list(self.get(pb.MESH_TOPIC_CHANNELS)[0].mesh_channels.channels)

    def command(self, **op):
        return self.link.expect_result(mesh=pb.MeshCommand(**op))

    def radio(self, on, announce=True):
        """Mesh on or off from the app. Turned on, waits (unless `announce`
        is False) for the NodeInfo the device sends 5 s later, so a test's
        own frames don't land while it is transmitting (half-duplex)."""
        seq = self.dev.lora()["tx_seq"]
        self.command(radio_on=on)
        want = "rx" if on else "sleep"
        self.dev_wait(lambda: self.dev.lora()["mode"] == want, "radio " + want)
        if on and announce:
            self.dev_wait(lambda: self.dev.lora()["tx_seq"] > seq and self.dev.lora()["mode"] == "rx",
                          "the announcement after switching on", timeout=15)

    def send(self, text, dest=BROADCAST, channel=0):
        """As the app's composer: the answer (mesh_queued or a result)."""
        return self.link.request(mesh=pb.MeshCommand(send=pb.MeshSend(dest=dest, channel=channel, text=text)))

    def set_private(self, index, name, psk):
        self.command(set_channel=pb.MeshChannelEdit(index=index, name=name, psk=psk))
        self.dev_wait(lambda: any(c.index == index and c.name == name for c in self.device_channels()),
                      "private channel %d applied" % index)
        self.channels.append(M.Channel(name, psk))

    # -- the air ---------------------------------------------------------------------------

    def hear(self, frame, **kw):
        """Puts a frame on the air for the device."""
        return self.dev.lora_rx(frame, **kw)

    def drain(self, timeout=10):
        """Waits until everything on the air has reached the device and the
        mesh task has had time to handle it (it polls every 250 ms)."""
        deadline = time.time() + timeout * TIME_SCALE
        while True:
            st = self.dev.lora()
            if st["in_flight"] == 0 and not st["fifo"]:
                break
            assert time.time() < deadline, "frames never left the air: %s" % st
            time.sleep(0.05)
        self.dev_sleep(400)

    def dev_sleep(self, device_ms):
        """Waits `device_ms` of device time."""
        t0 = self.dev.status()["uptime_ms"]
        while self.dev.status()["uptime_ms"] - t0 < device_ms:
            time.sleep(0.05)

    def dev_wait(self, pred, what, timeout=10):
        deadline = time.time() + timeout * TIME_SCALE
        while not pred():
            assert time.time() < deadline, "timed out waiting for " + what
            time.sleep(0.1)

    def sent(self, since=None, channels=None, pki_from=None, ear=None):
        """Frames the device transmitted since `since` (default: since the last
        call), decoded by a stock node (`ear`, default one that knows the
        device's channels). Each: header fields + data + `frame` + timing."""
        r = self.dev.cmd("lora_tx", since=self.tx_seq if since is None else since)
        # Only transmissions that have finished: one still going out is not
        # yet something a test may answer (the radio is deaf until it ends,
        # and until the mesh task — which polls every 250 ms — re-arms it).
        frames = []
        for f in r["frames"]:
            if f["end_ms"] + 300 > r["now_ms"]:
                break
            f["data"] = bytes.fromhex(f["hex"])
            frames.append(f)
        if since is None and frames:
            self.tx_seq = frames[-1]["seq"]
        out = []
        ear = ear or self.ear
        for f in frames:
            h = ear.decode(f["data"], channels=channels or self.channels,
                           pki_from=pki_from)
            h["frame"] = f["data"]
            h["tx"] = f
            out.append(h)
        return out

    def wait_sent(self, pred, what, timeout=15, **kw):
        """Waits for a transmission matching `pred` (on decoded frames)."""
        deadline = time.time() + timeout * TIME_SCALE
        seen = []
        while True:
            for h in self.sent(**kw):
                seen.append(h)
                if pred(h):
                    return h
            assert time.time() < deadline, "the device never sent %s; it sent %s" % (
                what, [(x["dest"], x["data"] and x["data"]["portnum"]) for x in seen])
            time.sleep(0.1)

    def close(self):
        try:
            self.phone.disconnect()
        except Exception:
            pass


def keep_artifacts(request, dev):
    failed = getattr(request.node, "rep_call", None) is not None and request.node.rep_call.failed
    if failed:
        out = os.path.join(ARTIFACTS, request.node.name + ("-" + os.path.basename(dev.dir)))
        os.makedirs(out, exist_ok=True)
        try:
            dev.screenshot(os.path.join(out, "panel.png"))
        except Exception:
            pass
    dev.stop()
    if failed:
        shutil.copy(dev.log_path, os.path.join(out, "console.log"))
    shutil.rmtree(dev.dir, ignore_errors=True)


@pytest.fixture
def mesh(request):
    """A device with its radio fitted and on (LongFast, the default channel),
    a paired phone on the link, and the phone's view of the mesh."""
    dev = start_device()
    m = None
    try:
        m = Mesh(dev)
        yield m
    finally:
        if m:
            m.close()
        keep_artifacts(request, dev)


def is_port(h, port):
    return h["data"] is not None and h["data"]["portnum"] == port


def lock_warnings(dev):
    return [l for l in dev.log() if "WITHOUT sdLock" in l or "LOCK ORDER INVERSION" in l]
