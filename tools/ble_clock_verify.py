"""Verify the isolated controller replacement before accepting its ELF."""
from pathlib import Path
import hashlib
import json

Import("env")

def verify(source, target, env):
    root = Path(env.subst("$PROJECT_DIR"))
    build = Path(env.subst("$BUILD_DIR"))
    upstream = root / "tools/ble_clock_vendor/bt.c"
    digest = hashlib.sha256(upstream.read_bytes()).hexdigest()
    if digest != "47625a3558e51afe0cf11588907228b0fe9bed9bdcdeca88e70e9ea4c01553ad":
        raise RuntimeError("Vendored IDF controller source fingerprint changed")
    linkmap = (build / "firmware.map").read_text()
    if "libbt.a(bt.c.obj)" in linkmap:
        raise RuntimeError("Original RC controller adapter was linked into XTAL build")
    if "board_bt_controller_init" not in linkmap or "ble_clock_xtal.c.o" not in linkmap:
        raise RuntimeError("XTAL controller adapter missing from link")
    (build / "ble-clock-profile.json").write_text(json.dumps({
        "idf_commit": "3572900934",
        "adapter_sha256": digest,
        "clock": "MAIN_XTAL",
        "retain_xtal_in_light_sleep": True,
        "original_controller_adapter_linked": False,
    }, indent=2) + "\n")
    print("BLE clock verified: matching IDF adapter, main XTAL retained, original adapter excluded")

env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", verify)
