#!/usr/bin/env python3
"""Gate the release entry point, not just the optional battery test profile.

PlatformIO resolves extends/interpolation before these checks. The build's
BLE link verifier and CI's PM symbol check validate the resulting binary.
"""
import json
from pathlib import Path
import shlex
import subprocess


def check(sections):
    config = {name: dict(options) for name, options in sections}
    if config["platformio"]["default_envs"] != ["t5s3-painter"]:
        raise ValueError("Default build must match the shipping artifact path")
    shipping = config["env:t5s3-painter"]
    tokens = shlex.split(" ".join(shipping["build_flags"]))
    required = {
        "PM_BLE_XTAL": "1",
        "PM_GPS_RX_GUARD": "1",
        "PM_GPS_LEAN_NMEA": "1",
        "PM_GPS_EVENT_RX": "1",
        "PM_SENSOR_SLEEP": "1",
        "ARDUINO_SERIAL_EVENT_TASK_STACK_SIZE": "4096",
    }
    for name, value in required.items():
        definitions = [t for t in tokens if t == "-D" + name or
                       t.startswith("-D" + name + "=") or t == "-U" + name]
        if definitions != [f"-D{name}={value}"]:
            raise ValueError(f"Shipping build requires {name}={value}: {definitions}")
    for name, value in {"PM_LIGHT_SLEEP": "1", "PM_MIN_CPU_MHZ": "240"}.items():
        overrides = [t for t in tokens if t.startswith("-D" + name + "=")]
        if any(t != f"-D{name}={value}" for t in overrides):
            raise ValueError(f"Shipping power policy overridden: {overrides}")
    if "post:tools/ble_clock_verify.py" not in shipping["extra_scripts"]:
        raise ValueError("Shipping build must verify the linked BLE clock adapter")
    alias = config["env:t5s3-painter-battery-opt"]
    for option in ("build_flags", "extra_scripts", "build_src_filter"):
        if shipping[option] != alias[option]:
            raise ValueError(f"Battery compatibility alias differs from shipping: {option}")


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[2]
    result = subprocess.run(["pio", "project", "config", "--json-output"],
                            cwd=root, check=True, text=True, capture_output=True)
    check(json.loads(result.stdout))
    print("Shipping battery configuration verified (default build and compatibility alias)")
