"""Fixed pairing (src/ble_server.cpp): one phone owns the device."""
import pytest

from conftest import paired_phone
from otpsim import Disconnected, GattError, LinkClient, SimPhone

STATUS_CHR = "b1c50002-9e0f-4b7a-9c6d-1f2e3a4b5c6d"


def test_first_phone_pairs_and_owns_the_device(device):
    phone = paired_phone(device)
    st = device.wait_status(lambda s: s["phone_connected"] and s["phone_paired"])
    assert st["ble"]["bonds"] == [phone.identity]
    # The phone's GAP name, read over the admitted link by the AMS client.
    device.wait_status(lambda s: s["phone_name"] == "Sim iPhone")
    phone.disconnect()
    device.wait_status(lambda s: not s["phone_connected"])
    adv = SimPhone.scan(device.ble_port)
    assert adv and adv["name"] == "OpenTrailPaper" and adv["paired"], adv


def test_the_paired_phone_reconnects_without_pairing(device):
    phone = paired_phone(device)
    phone.disconnect()
    device.wait_status(lambda s: not s["phone_connected"])
    phone.connect()
    sec = phone.wait_security(10)
    assert sec["encrypted"] and sec["bonded"]
    device.wait_status(lambda s: s["phone_connected"])
    assert len(device.status()["ble"]["bonds"]) == 1


def test_a_second_phone_is_refused(device):
    owner = paired_phone(device)
    owner.disconnect()
    other = SimPhone(device.ble_port, identity="d0:00:00:00:00:02", name="Other phone").connect()
    reason = other.wait_disconnected(10)
    assert reason == 0x05   # the firmware's refusal (BLE_ERR_AUTH_FAIL)
    st = device.status()
    assert st["ble"]["bonds"] == [owner.identity]
    device.wait_log(r"refusing phone link", since=0)


def test_a_phone_that_forgot_the_device_is_refused_and_keeps_the_bond(device):
    phone = paired_phone(device)
    phone.disconnect()
    phone.bonded = False          # "Forget This Device" in iOS Settings
    phone.connect()
    phone.wait_disconnected(10)
    assert device.status()["ble"]["bonds"] == [phone.identity]


def test_stale_keys_on_the_phone_are_refused(device):
    """The device was unpaired on the panel; the phone still holds keys."""
    phone = paired_phone(device)
    link = LinkClient(phone).hello()
    link.tell(unpair=__import__("otpsim").pb.Unpair())
    phone.wait_disconnected(10)
    device.wait_status(lambda s: not s["ble"]["bonds"])
    phone.connect()                # still "bonded" on the phone side
    sec = phone.wait_security(10)
    assert not sec["encrypted"]
    # Unpaired, the device keeps the unencrypted link open for pairing again.
    with pytest.raises(GattError):
        phone.read(STATUS_CHR)


def test_unpair_from_the_app_opens_pairing_to_the_next_phone(device):
    phone = paired_phone(device)
    link = LinkClient(phone).hello()
    link.tell(unpair=__import__("otpsim").pb.Unpair())
    phone.wait_disconnected(10)
    device.wait_status(lambda s: not s["ble"]["bonds"] and not s["phone_paired"])
    other = paired_phone(device, identity="d0:00:00:00:00:03", name="New phone")
    assert device.wait_status(lambda s: s["phone_connected"])["ble"]["bonds"] == [other.identity]


def test_protected_attributes_need_encryption(device):
    phone = SimPhone(device.ble_port).connect(pair=False)
    with pytest.raises(GattError) as e:
        phone.read(STATUS_CHR)
    assert e.value.status in (0x05, 0x0F)


def test_a_refused_phone_that_starts_pairing_gets_no_bond(device):
    """A stranger that asks for security at once must not leave a bond behind."""
    owner = paired_phone(device)
    owner.disconnect()
    other = SimPhone(device.ble_port, identity="d0:00:00:00:00:04")
    other.connect()
    try:
        other._send(0x06)   # SECURE straight away
    except OSError:
        pass
    other.wait_disconnected(10)
    assert device.status()["ble"]["bonds"] == [owner.identity]
