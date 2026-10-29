"""Things going wrong mid-ride: the phone's radio link drops, a sensor goes
quiet and comes back, the card is pulled."""
import time

from conftest import paired_phone
from otpsim import LinkClient, SimDevice, SimPhone, pb


def test_the_phone_link_drops_and_comes_back(device, phone):
    link = LinkClient(phone).hello()
    device.drop_phone(0x08)   # supervision timeout
    phone.wait_disconnected(5)
    st = device.wait_status(lambda s: not s["phone_connected"] and s["ble"]["advertising"], timeout=10)
    assert st["phone_paired"]
    phone.connect()
    assert phone.wait_security(10)["encrypted"]
    link = LinkClient(phone).hello()
    link.wait_push("live", timeout=5)
    device.wait_status(lambda s: s["phone_connected"])


def test_a_stale_link_is_replaced_by_the_paired_phone(device, phone):
    """iOS reconnects while the device still thinks the old link is up."""
    LinkClient(phone).hello()
    again = SimPhone(device.ble_port, identity=phone.identity, bonded=True).connect()   # the same phone
    assert again.wait_security(10)["encrypted"]
    LinkClient(again).hello()
    phone.wait_disconnected(10)     # the device dropped the old one
    device.wait_log(r"stale link", since=0, timeout=5)


def test_a_sensor_that_drops_out_reconnects(device, link):
    addr = device.sensor("hr", bpm=130)
    link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_SCAN_START))
    link.wait_push("sensor_list", lambda l: any(s.address == addr for s in l.sensors), timeout=10)
    link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_PAIR, address=addr))
    link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_SCAN_STOP))
    device.wait_status(lambda s: s["ride"]["hr"] == 130, timeout=20)
    device.cmd("sensor", kind="hr", addr=addr, drop=True)          # out of range, still advertising
    device.wait_status(lambda s: s["ride"]["hr"] == -1, timeout=30)
    device.cmd("sensor", kind="hr", addr=addr, bpm=135)
    device.wait_status(lambda s: s["ride"]["hr"] == 135, timeout=60)


def test_no_card_is_reported_to_the_app():
    dev = SimDevice(args=["--gps-baud", "115200"])
    import os
    os.makedirs(os.path.join(dev.dir, "sd"), exist_ok=True)
    try:
        dev.launch()
        dev.set_sd(False)
        dev.reboot()
        phone = paired_phone(dev)
        link = LinkClient(phone).hello()
        frame = link.wait_push("live", lambda f: f.ride.sd_card == pb.SD_CARD_MISSING, timeout=15)
        assert frame.ride.sd_card == pb.SD_CARD_MISSING
    finally:
        dev.stop()


def test_the_card_pulled_mid_session_refuses_downloads(device, link):
    device.set_sd(False)
    r = link.request(list=pb.ListRequest(kind=pb.LIST_RIDES))
    body = r.WhichOneof("body")
    assert body == "result" and r.result.code in (pb.RESULT_NO_SD, pb.RESULT_IO_ERROR) or \
        body == "file_list" and not r.file_list.files
