"""Regressions: past bugs in the phone link, fixed pairing and BLE sensors,
each replayed against the sim. Every test names the commit that fixed it and
was checked to fail with that fix reverted (by hand, in today's src/) and to
pass with it in place."""
import os
import time

import pytest

from conftest import paired_phone
from otpsim import Disconnected, LinkClient, SimPhone, pb

OWNER = "d0:00:00:00:00:01"
STRANGER = "d0:00:00:00:00:02"


def pair_sensor(link, addr):
    """As the app's Sensors sheet does it: scan, see it, pair it."""
    link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_SCAN_START))
    link.wait_push("sensor_list", lambda l: any(s.address == addr for s in l.sensors), timeout=10)
    r = link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_PAIR, address=addr))
    link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_SCAN_STOP))
    assert any(s.address == addr and s.paired for s in r.sensor_list.sensors), r


def eventually(device, predicate, what, timeout=10):
    deadline = time.time() + timeout
    st = device.status()
    while not predicate(st) and time.time() < deadline:
        time.sleep(0.2)
        st = device.status()
    assert predicate(st), what
    return st


def still_admitted(device, link):
    """The paired phone's link session is alive: the device says the phone is
    connected, answers a request and keeps pushing live frames."""
    st = device.status()
    assert st["phone_connected"], "the device let go of the paired phone"
    assert link.request(get_settings=pb.GetSettings(), timeout=3).WhichOneof("body") == "settings"
    link.drain_pushes()
    link.wait_push("live", timeout=5)


# ---- fixed pairing ---------------------------------------------------------------


def test_eb8a337_a_second_phone_cannot_pair_while_paired(device):
    """eb8a337 (#115) Fixed phone pairing: before it, any phone in range could
    pair with the device and take it over — the device belonged to whoever
    paired last."""
    owner = paired_phone(device, identity=OWNER)
    owner.disconnect()
    device.wait_status(lambda s: not s["phone_connected"])
    other = SimPhone(device.ble_port, identity=STRANGER, name="Someone else").connect()
    try:
        sec = other.wait_security(10)
    except Disconnected:
        sec = None
    assert not (sec and sec["encrypted"]), "a second phone paired and took the device: %s" % sec
    assert other.wait_disconnected(10) == 0x05
    st = device.status()
    assert st["ble"]["bonds"] == [OWNER], st["ble"]["bonds"]
    assert st["phone_paired"]


def test_05c458c_a_second_phone_gets_no_passkey_dialog(device):
    """05c458c (#124): while paired, a stranger was only refused at the passkey
    step — after the device's security request had already made the phone
    start pairing and put a passkey dialog up that the panel never answered
    (the 2026-10-10 loop). It must be dropped on connect, before any dialog.
    Needs the sim's phone to raise its dialog when pairing starts (as iOS
    does), not after the firmware's passkey callback."""
    owner = paired_phone(device, identity=OWNER)
    owner.disconnect()
    device.wait_status(lambda s: not s["phone_connected"])
    other = SimPhone(device.ble_port, identity=STRANGER, name="Someone else")
    other.connect(auto_passkey=False)
    reason = other.wait_disconnected(10)
    assert not other._passkey_req.is_set(), "the refused phone was shown a passkey dialog"
    assert reason == 0x05
    assert device.status()["ble"]["bonds"] == [OWNER]


def test_05c458c_the_paired_phone_re_pairing_keeps_its_bond(device):
    """05c458c (#124): the paired phone that forgot the device ("Forget This
    Device") asks to pair again. NimBLE-Arduino's own handler deleted the
    paired phone's bond before anything could refuse — leaving the device
    unpaired, open to the next phone. The pairing guard must refuse it first.
    Needs the sim's model of NimBLE-Arduino's repeat-pairing default."""
    phone = paired_phone(device, identity=OWNER)
    phone.disconnect()
    device.wait_status(lambda s: not s["phone_connected"])
    phone.bonded = False
    phone.connect()
    phone.wait_disconnected(10)
    st = device.status()
    assert st["ble"]["bonds"] == [OWNER], "the paired phone's bond was deleted: %s" % st["ble"]["bonds"]
    assert st["phone_paired"]
    # Still paired: a stranger is still turned away.
    other = SimPhone(device.ble_port, identity=STRANGER).connect()
    other.wait_disconnected(10)
    assert device.status()["ble"]["bonds"] == [OWNER]


def test_05c458c_failed_encryption_keeps_the_bond(device):
    """05c458c (#124): a link from the bonded identity whose encryption failed
    (keys that don't match) made the device drop the paired phone's bond as
    'stale' — unpairing it, so any phone could then pair. The bond stays; the
    link goes. Needs the sim's stale-keys connect flag."""
    phone = paired_phone(device, identity=OWNER)
    phone.disconnect()
    device.wait_status(lambda s: not s["phone_connected"])
    phone.connect(stale_keys=True)
    sec = phone.wait_security(10)
    assert not sec["encrypted"]
    phone.wait_disconnected(10)
    st = device.status()
    assert st["ble"]["bonds"] == [OWNER], "failed encryption deleted the bond: %s" % st["ble"]["bonds"]
    assert st["phone_paired"]
    # The real phone still gets straight back in.
    phone.connect()
    assert phone.wait_security(10)["encrypted"]
    device.wait_status(lambda s: s["phone_connected"])


def test_05c458c_unpair_deletes_the_bond_while_scanning(device):
    """05c458c (#124): ble_gap_unpair refuses (BLE_HS_EBUSY) to delete a bond
    with an IRK while advertising or scanning — and the device was usually
    doing one of them (a sensor hunt, a stranger's re-advertising). Unpair
    logged '0 bond(s) deleted' and the phone stayed paired. Needs the sim's
    EBUSY model of ble_gap_unpair."""
    phone = paired_phone(device, identity=OWNER)
    link = LinkClient(phone).hello()
    addr = device.sensor("hr", bpm=120)
    pair_sensor(link, addr)
    device.wait_status(lambda s: s["ride"]["hr"] == 120, timeout=20)
    device.remove_sensor(addr)       # out of range: the device hunts for it
    device.wait_status(lambda s: s["ble"]["scanning"], timeout=15)
    link.tell(unpair=pb.Unpair())
    phone.wait_disconnected(10)
    deadline = time.time() + 10
    st = device.status()
    while (st["ble"]["bonds"] or st["phone_paired"]) and time.time() < deadline:
        time.sleep(0.2)
        st = device.status()
    assert st["ble"]["bonds"] == [], "unpair left the phone's bond: %s" % st["ble"]["bonds"]
    assert not st["phone_paired"]
    # And it means it: the phone's old keys no longer get it in.
    phone.connect(pair=False)
    assert not phone.wait_security(10)["encrypted"]


def test_05c458c_a_stranger_does_not_unseat_the_connected_phone(device, phone):
    """05c458c (#124): a second central connecting while the paired phone was
    connected overwrote the device's one phone slot, and its refusal's
    disconnect then reset the paired phone's state — the app's session died
    mid-use while its link was still up (a stranger reconnecting three times a
    second, 2026-10-10)."""
    link = LinkClient(phone).hello()
    link.wait_push("live", timeout=5)
    for _ in range(3):
        other = SimPhone(device.ble_port, identity=STRANGER, name="Someone else").connect()
        assert other.wait_disconnected(10) == 0x05
    time.sleep(0.5)
    still_admitted(device, link)


def refresh_keys(phone):
    phone._security.clear()
    phone._send(0x06)                # SECURE on an encrypted link: a key refresh
    assert phone.wait_security(5)["encrypted"]
    time.sleep(0.3)


def test_69c5f86_a_key_refresh_keeps_the_link_session(device, phone):
    """69c5f86: a key refresh on the admitted link raises another
    authentication-complete; the device took it for a new admission and reset
    the link session, so the app's requests went unanswered until it said
    Hello again. Needs the sim's key refresh (SECURE on an encrypted link)."""
    phone.disconnect()               # the paired phone, back another day
    device.wait_status(lambda s: not s["phone_connected"])
    phone.connect()
    assert phone.wait_security(10)["encrypted"]
    link = LinkClient(phone).hello()
    link.wait_push("live", timeout=5)
    refresh_keys(phone)
    still_admitted(device, link)
    assert device.status()["ble"]["bonds"] == [phone.identity]


@pytest.mark.xfail(strict=True, reason="new bug: on the link a phone just paired over, a second "
                   "authentication-complete (a key refresh) is taken for 'a new bond while paired' — "
                   "onAuthenticationComplete rejects it before admit()'s 69c5f86 guard, deleting the "
                   "phone's bond and dropping it")
def test_a_key_refresh_on_the_pairing_link_keeps_the_new_phone(device, phone):
    """The 69c5f86 guard sits in admit(); a link whose phone paired on it
    (linkPeerKnown false) never reaches it on the second event."""
    link = LinkClient(phone).hello()
    link.wait_push("live", timeout=5)
    refresh_keys(phone)
    assert device.status()["ble"]["bonds"] == [phone.identity], "the new phone's bond was deleted"
    still_admitted(device, link)


def test_868ace8_a_phone_paired_before_an_update_is_told_to_rediscover(device):
    """868ace8: iOS reuses a bonded device's cached GATT table, so services a
    firmware update added (the live stream) never appeared for a phone paired
    before it. The device must indicate Service Changed after the reboot that
    ends every update — once the phone's subscriptions are restored, or the
    indication reaches nobody (the first try, sent from
    onAuthenticationComplete). Needs the sim's restored-CCCD gate."""
    phone = paired_phone(device, identity=OWNER)
    device.reboot()                  # the firmware update's reboot
    phone.wait_disconnected(10)
    phone.connect()
    assert phone.wait_security(10)["encrypted"]
    assert phone.service_changed.wait(8), "no Service Changed after the update"


# ---- the link -----------------------------------------------------------------------


def test_ea06a72_the_device_keeps_each_tiles_version_record(device, link):
    """ea06a72 (#129): only the phone that sent a map hex knew which version it
    was, so hexes sent from another phone or before a reinstall showed as
    'update available' when identical. The device keeps the record sent with
    each tile and lists it back — to any session, across reboots."""
    tile = b"EBM2" + bytes(60)
    rec = "cdn:0123456789abcdef"
    link.upload(pb.TARGET_MAP_TILE, "8828308281fffff", tile, tile_version=rec)

    def versions(lk):
        pages = lk.request(list=pb.ListRequest(kind=pb.LIST_MAP_TILES), all_pages=True)
        return {t.h3: t.version for p in pages for t in p.tile_list.tiles}

    assert versions(link).get("8828308281fffff") == rec
    device.reboot()                  # a day later, the app reinstalled
    phone = link.phone
    phone.wait_disconnected(10)
    phone.connect()
    assert phone.wait_security(10)["encrypted"]
    assert versions(LinkClient(phone).hello()).get("8828308281fffff") == rec


def test_69c5f86_a_resume_past_the_end_restarts_the_download(device, link):
    """69c5f86: a download resumed from an offset past the file's end (the
    file changed under the phone) was refused with BAD_REQUEST, so the app
    could never fetch it again. The device starts over from 0."""
    os.makedirs(os.path.join(device.sd, "rides"), exist_ok=True)
    blob = os.urandom(5000)
    with open(os.path.join(device.sd, "rides", "20260101-080000.fit"), "wb") as f:
        f.write(blob)
    data = link.download(pb.SOURCE_RIDE, "20260101-080000.fit", have=os.urandom(9000))
    assert link.last_download_begin.offset == 0
    assert data == blob


# ---- BLE sensors -------------------------------------------------------------------


def test_40fcc5f_a_factory_fresh_device_scans_for_its_first_sensor(device, link):
    """40fcc5f: the app's sensor scan was ANDed with 'a paired sensor is
    missing' — vacuously false with nothing paired — so once the boot-time
    90 s window was over, a factory-fresh device never scanned and could
    never pair its first sensor."""
    device.set_speed(30)
    device.wait_status(lambda s: s["uptime_ms"] > 95000, timeout=30)
    device.set_speed(1)
    addr = device.sensor("hr", bpm=110)
    link.request(sensors=pb.SensorCommand(op=pb.SENSOR_OP_SCAN_START))
    eventually(device, lambda s: s["ble"]["scanning"], "the app asked for a scan and the device isn't scanning", 5)
    link.wait_push("sensor_list", lambda l: any(s.address == addr for s in l.sensors), timeout=10)


def test_bdbec8d_a_power_meter_at_a_standstill_is_a_cadence_source(device, link):
    """bdbec8d: a power meter with crank data counted as a cadence source only
    while a cadence could be computed. Standing still it sends revs=0, time=0
    forever, so cadence read '--' until the rider pedalled; and when the
    pedalling stopped the last cadence was held instead of 0."""
    addr = device.sensor("power", watts=0, cadence=0)
    pair_sensor(link, addr)
    device.wait_status(lambda s: s["ride"]["power"] == 0, timeout=20)
    eventually(device, lambda s: s["ride"]["cadence"] == 0, "a still crank reads cadence '--', not 0")
    device.cmd("sensor", kind="power", addr=addr, watts=200, cadence=85)
    device.wait_status(lambda s: 80 <= s["ride"]["cadence"] <= 90, timeout=15)
    device.cmd("sensor", kind="power", addr=addr, watts=0, cadence=0)
    eventually(device, lambda s: s["ride"]["cadence"] == 0, "the last cadence is held after the crank stopped")
