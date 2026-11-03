#!/usr/bin/env python3
"""Send bench commands and print the replies, saving them to a log.

    console.py <logfile> <seconds to wait after each command> <command>...

Needs pyserial. A `run` or `video` command is waited for until it reports it has
finished, however long that takes, then its last frame gets those seconds on the
panel before the next command goes out.
"""
import glob
import sys
import time

import serial


def port():
    for p in sorted(glob.glob('/dev/cu.usbmodem*')):
        if 'NTFAAN' not in p:   # the EDIFIER speaker
            return p
    raise SystemExit('no board')


FINISHED = ('look for ghosts', ' done')


def drain(s, log, quiet, until_finished=False):
    last = time.monotonic()
    while until_finished or time.monotonic() - last < quiet:
        line = s.readline()
        if not line:
            continue
        text = line.decode('utf-8', 'replace').rstrip()
        print(text, flush=True)
        log.write(text + '\n')
        log.flush()
        if until_finished and any(f in text for f in FINISHED):
            until_finished = False
            last = time.monotonic()


def main():
    logfile, quiet, commands = sys.argv[1], float(sys.argv[2]), sys.argv[3:]
    with serial.Serial(port(), 115200, timeout=0.5) as s, open(logfile, 'a') as log:
        s.write(b'\nhelp\n')
        drain(s, log, 2)
        for c in commands:
            print(f'>>> {c}', flush=True)
            log.write(f'>>> {c}\n')
            s.write(c.encode() + b'\n')
            drain(s, log, quiet, c.split()[0] in ('run', 'video', 'quick', 'compare', 'paced'))


main()
