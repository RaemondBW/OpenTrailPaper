#!/usr/bin/env python3
"""Hands-free USB flashing for the T5S3 bike computer.

The board runs its USB in OTG (TinyUSB) mode, so esptool's auto-reset can't
reach it: while the app is running there is no UART bridge whose DTR/RTS lines
are wired to GPIO0/EN. Getting firmware onto it has therefore cost two button
presses -- one to enter download mode, one to leave it. This removes both.

Entering is the easy half and already existed: the firmware's `bootloader`
console command (rebootToBootloader() in src/ui_dashboard.cpp) reboots into the
ROM loader, which comes back as the USB-Serial-JTAG device -- a *different*
serial port from the app's OTG one, with "JTAG" in its USB product string.

*Leaving* download mode is what needed the RESET tap, for two independent
reasons, both of which this script handles:

  1. usb_persist_restart(RESTART_BOOTLOADER) sets RTC_CNTL_FORCE_DOWNLOAD_BOOT
     (esp32-hal-tinyusb.c) and nothing in the Arduino core ever clears it. The
     bit lives in the RTC domain, which a soft reset does not touch, so every
     later reset walks straight back into download mode. Only pulling CHIP_EN
     low -- the physical RESET button -- resets that domain. So we clear the bit
     ourselves, over the download-mode connection, before resetting.

  2. esptool's own `--after hard_reset` is wrong for this bridge. HardReset
     pulses RTS only, and its _setRTS() helper re-asserts the port's current DTR
     as a Windows workaround. On USB-Serial-JTAG, DTR drives GPIO0 -- and
     pyserial asserts DTR on open, while `--before no_reset` (which we must use,
     the board being in download mode already) never runs the JTAG sequence that
     would clear it. The reset thus releases EN with GPIO0 still low, which is
     download mode again. So we drive the reset ourselves with DTR explicitly
     deasserted.

Usage:
    tools/flash.py                      # enter download mode, flash, reset
    tools/flash.py --firmware fw.bin    # ... a specific image
    tools/flash.py enter                # only reboot into download mode
    tools/flash.py finish               # only clear the bit and reset

This is also the engine behind `pio run -t upload`; see tools/pio_upload.py.
"""

from __future__ import annotations

import argparse
import os
import sys
import time

try:
    import serial
    import serial.tools.list_ports as list_ports
except ImportError:  # pragma: no cover - pyserial ships with PlatformIO
    sys.exit("pyserial is required: pip install pyserial")


# --- chip constants ---------------------------------------------------------
# soc/rtc_cntl_reg.h: DR_REG_RTCCNTL_BASE (0x60008000) + 0x12C, bit 0.
RTC_CNTL_OPTION1_REG = 0x6000812C
RTC_CNTL_FORCE_DOWNLOAD_BOOT = 0x1

ESPRESSIF_VID = 0x303A

# How long the ROM loader takes to enumerate after the `bootloader` command, and
# how long the app's TinyUSB CDC takes to come back after the reset. Both are
# generous: a slow SD mount at boot can push the app port out past 5 s.
DOWNLOAD_PORT_TIMEOUT = 20.0
# Generous, because the first boot after a flash is the slowest one there is: a
# board recovering from a crash loop repairs the ride file it was cut off in the
# middle of before the console ever comes up, which has been measured past 25 s.
# Overshooting only delays a warning on a board that never comes back;
# undershooting cries wolf on a perfectly good flash.
APP_PORT_TIMEOUT = 60.0


def _log(msg: str) -> None:
    print(f"[flash] {msg}", flush=True)


# --- port discovery ---------------------------------------------------------
# Both the app's TinyUSB CDC and the ROM's USB-Serial-JTAG enumerate as
# 303a:1001, so the PID cannot tell them apart. The product string can: the ROM
# device reports "USB JTAG/serial debug unit", the app reports the board name
# from the Arduino USB descriptors ("LilyGo T5-ePaper-S3").
def esp_ports():
    return [p for p in list_ports.comports() if p.vid == ESPRESSIF_VID]


def is_rom_port(p) -> bool:
    return "jtag" in (p.product or "").lower()


def find_rom_port():
    for p in esp_ports():
        if is_rom_port(p):
            return p.device
    return None


def find_app_port():
    for p in esp_ports():
        if not is_rom_port(p):
            return p.device
    return None


def _wait_for(predicate, timeout: float, what: str):
    """Poll the port list until `predicate` returns a device path."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        found = predicate()
        if found:
            # macOS creates the /dev node a moment before the CDC endpoints are
            # actually usable; opening too eagerly gets ENXIO.
            time.sleep(0.4)
            return found
        time.sleep(0.2)
    raise SystemExit(f"[flash] timed out after {timeout:.0f}s waiting for {what}")


# --- download mode ----------------------------------------------------------
def enter_download_mode() -> str:
    """Get the board into the ROM loader and return its serial port."""
    existing = find_rom_port()
    if existing:
        _log(f"already in download mode on {existing}")
        return existing

    _log("asking the firmware to reboot into download mode")

    # Keep trying for the whole window rather than sampling once. Two things make
    # a single attempt unreliable, and both are worst on a board that is
    # crash-looping — precisely when you most need to reflash it:
    #   - the port enumerates a second or two into boot, before the console task
    #     is necessarily reading it, so an early write is simply dropped;
    #   - between resets the port is not there at all, so a one-shot lookup can
    #     conclude "no board" about a board that is plugged in and fine.
    # Re-sending costs nothing: the command is idempotent, and once the ROM
    # loader is up the app port is gone and we stop.
    deadline = time.monotonic() + DOWNLOAD_PORT_TIMEOUT
    last_err = None
    while time.monotonic() < deadline:
        rom = find_rom_port()
        if rom:
            time.sleep(0.4)   # let the CDC endpoints settle before esptool opens it
            _log(f"download mode up on {rom}")
            return rom
        app = find_app_port()
        if app:
            try:
                # The console runs at 115200 but this is a native CDC port, so the
                # baud rate is decorative. The leading newline flushes any
                # half-typed line.
                with serial.Serial(app, 115200, timeout=1) as s:
                    s.write(b"\nbootloader\n")
                    s.flush()
            except (serial.SerialException, OSError) as e:
                # Expected while the board is mid-reset; only worth reporting if
                # we never get in at all.
                last_err = e
        time.sleep(0.5)

    hint = f" (last error: {last_err})" if last_err else ""
    raise SystemExit(
        f"[flash] the firmware never entered download mode{hint}.\n"
        "        Close any serial monitor, or do it by hand: hold BOOT, tap\n"
        "        RESET, release BOOT, then re-run."
    )


# --- esptool plumbing -------------------------------------------------------
def _esptool(args: list[str]) -> None:
    """Run esptool in-process, preferring PlatformIO's pinned copy."""
    try:
        import esptool  # noqa: F401
    except ImportError:
        pio_esptool = os.path.expanduser("~/.platformio/packages/tool-esptoolpy")
        if not os.path.isdir(pio_esptool):
            raise SystemExit("[flash] esptool not importable: pip install esptool")
        sys.path.insert(0, pio_esptool)
        import esptool  # noqa: F811

    esptool.main(args)


def clear_force_download_boot(port: str) -> None:
    """Clear the sticky bit that would send the next reset back to download mode.

    Runs over the still-open download-mode connection. `--before no_reset`
    because the board is already in the ROM loader and must not be bounced;
    `--after no_reset` because we do the reset ourselves, correctly, below.
    """
    _log("clearing RTC_CNTL_FORCE_DOWNLOAD_BOOT")
    _esptool([
        "--chip", "esp32s3",
        "--port", port,
        "--before", "no_reset",
        "--after", "no_reset",
        "write_mem",
        hex(RTC_CNTL_OPTION1_REG),
        "0x0",
        hex(RTC_CNTL_FORCE_DOWNLOAD_BOOT),   # mask: touch only bit 0
    ])


def reset_into_app(port: str) -> None:
    """Reset the chip with GPIO0 held high, so it boots the app.

    On the USB-Serial-JTAG bridge DTR drives GPIO0 and RTS drives EN. pyserial
    asserts both on open, so the states are set *before* open() rather than
    after -- otherwise the chip would take a stray EN-low/GPIO0-low glitch (i.e.
    a bounce into download mode) on the way through.
    """
    _log("resetting into the application")
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.dtr = False    # GPIO0 high -- boot from flash, not the ROM loader
    s.rts = False
    s.open()
    try:
        s.rts = True     # EN low: chip in reset
        time.sleep(0.1)
        s.rts = False    # EN high: boots, sampling GPIO0 high
    finally:
        s.close()


def wait_for_app(timeout: float = APP_PORT_TIMEOUT) -> str | None:
    """Wait for the app's OTG console port to come back after the reset."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        # Ignore a ROM port lingering in the list: on macOS the old node can
        # take a beat to disappear after the device re-enumerates.
        app = find_app_port()
        if app:
            _log(f"application is up on {app}")
            return app
        time.sleep(0.2)
    _log(f"warning: the app console port did not reappear within {timeout:.0f}s")
    return None


def finish(port: str) -> None:
    """The whole after-flash half: clear the bit, reset, confirm the app is up."""
    clear_force_download_boot(port)
    reset_into_app(port)
    wait_for_app()


# --- flashing ---------------------------------------------------------------
def default_firmware() -> str:
    """The shipping env's build output (see default_envs in platformio.ini)."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(root, ".pio", "build", "t5s3-painter", "firmware.bin")


def write_firmware(port: str, firmware: str, address: int, baud: int) -> None:
    if not os.path.isfile(firmware):
        raise SystemExit(f"[flash] no such firmware image: {firmware}\n"
                         "        Build it first with `pio run`.")
    size = os.path.getsize(firmware)
    _log(f"writing {firmware} ({size} bytes) to {hex(address)}")
    _esptool([
        "--chip", "esp32s3",
        "--port", port,
        "--baud", str(baud),
        "--before", "no_reset",
        "--after", "no_reset",
        # Underscored spellings: esptool renamed these to hyphens in v5, but the
        # pinned PlatformIO copy is v4.5.1, where only these parse.
        "write_flash", "-z",
        "--flash_mode", "keep",
        "--flash_freq", "keep",
        "--flash_size", "keep",
        hex(address), firmware,
    ])


# --- CLI --------------------------------------------------------------------
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Flash the T5S3 bike computer without touching BOOT or RESET.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("command", nargs="?", default="flash",
                    choices=["flash", "enter", "finish"],
                    help="flash (default): enter download mode, write, reset. "
                         "enter: only reboot into download mode. "
                         "finish: only clear the bit and reset into the app.")
    ap.add_argument("--firmware", default=None,
                    help="image to write (default: .pio/build/t5s3-painter/firmware.bin)")
    ap.add_argument("--address", type=lambda x: int(x, 0), default=0x10000,
                    help="flash offset for the image (default: 0x10000, the app partition)")
    ap.add_argument("--baud", type=int, default=921600, help="upload baud rate")
    ap.add_argument("--port", default=None,
                    help="override port autodetection (the download-mode port)")
    args = ap.parse_args(argv)

    if args.command == "finish":
        port = args.port or find_rom_port()
        if not port:
            raise SystemExit("[flash] no board in download mode to finish.")
        finish(port)
        return 0

    port = args.port or enter_download_mode()
    if args.command == "enter":
        return 0

    write_firmware(port, args.firmware or default_firmware(), args.address, args.baud)
    finish(port)
    return 0


if __name__ == "__main__":
    sys.exit(main())
