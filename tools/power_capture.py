#!/usr/bin/env python3
"""Capture battery/SD evidence through one stable native-USB CDC connection.

Uses the app's existing port discovery. Never reconnects or cycles DTR/RTS.
USB power intentionally holds light sleep off: unplug for the sdtest phase,
then run this again to retrieve retained evidence with the diag command.
Completed test history that already flushed must be read from the SD daily log.
"""
import argparse
import datetime
import time
from pathlib import Path
import serial
from flash import find_app_port, esp_ports, is_rom_port


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port')
    parser.add_argument('--seconds', type=float, default=20)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--command', action='append', help='console command; defaults to sd, pm, diag')
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error('--seconds must be positive')
    port = args.port or find_app_port()
    if not port:
        parser.error('no application CDC port; connect the board or pass --port')
    if any(p.device == port and is_rom_port(p) for p in esp_ports()):
        parser.error('this is the ROM bootloader port, not the application console')
    commands = args.command or ['sd', 'pm', 'diag']
    # Exclusive output creation avoids accidentally replacing a previous run.
    with args.output.open('xb') as log:
        log.write(f'# {datetime.datetime.now(datetime.timezone.utc).isoformat()} {port}\n'.encode())
        stream = serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=2)
        stream.dtr = True
        stream.rts = True
        stream.port = port
        try:
            stream.open()
            deadline = time.monotonic() + args.seconds
            next_command = time.monotonic() + 1
            while time.monotonic() < deadline:
                if commands and time.monotonic() >= next_command:
                    command = commands.pop(0)
                    stream.write(('\n' + command + '\n').encode())
                    log.write(f'# command: {command}\n'.encode())
                    next_command = time.monotonic() + 2
                data = stream.read(4096)
                if data:
                    log.write(data)
                    log.flush()
                    print(data.decode('utf-8', errors='replace'), end='', flush=True)
        except (serial.SerialException, OSError) as error:
            log.write(f'\n# connection ended: {error}; no reconnect attempted\n'.encode())
            raise SystemExit(str(error)) from error
        finally:
            stream.close()


if __name__ == '__main__':
    main()
