"""Fixtures for the firmware sim tests (sim/README.md).

Every test gets a factory-fresh device (its own NVS, bonds and SD card in a
temp directory, on free ports). On failure the device's console log and a
screenshot of the panel land in sim/build/test-artifacts/<test>/."""
import os
import shutil
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "client"))

from otpsim import LinkClient, SimDevice, SimPhone, pb  # noqa: E402

ARTIFACTS = os.path.join(os.path.dirname(__file__), "..", "build", "test-artifacts")


@pytest.hookimpl(tryfirst=True, hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    rep = outcome.get_result()
    setattr(item, "rep_" + rep.when, rep)


@pytest.fixture
def device(request):
    """A booted device. Its GPS module starts at the firmware's own baud
    (115200) so boot doesn't spend 4 s probing; tests that care pass
    `gps_baud` via indirect parametrisation or start their own."""
    args = getattr(request, "param", None) or ["--gps-baud", "115200"]
    dev = SimDevice.start(args=args)
    yield dev
    failed = getattr(request.node, "rep_call", None) is not None and request.node.rep_call.failed
    if failed:
        out = os.path.join(ARTIFACTS, request.node.name)
        os.makedirs(out, exist_ok=True)
        try:
            dev.screenshot(os.path.join(out, "panel.png"))
        except Exception:
            pass
    dev.stop()
    if failed:
        shutil.copy(dev.log_path, os.path.join(out, "console.log"))
    shutil.rmtree(dev.dir, ignore_errors=True)


def paired_phone(device, identity="d0:00:00:00:00:01", name="Sim iPhone"):
    phone = SimPhone(device.ble_port, identity=identity, name=name).connect()
    sec = phone.wait_security(10)
    assert sec["encrypted"] and sec["bonded"], sec
    return phone


@pytest.fixture
def phone(device):
    p = paired_phone(device)
    yield p
    p.disconnect()


@pytest.fixture
def link(phone):
    return LinkClient(phone).hello()


__all__ = ["paired_phone", "pb"]
