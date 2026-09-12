#!/usr/bin/env python3
"""Decode a complete `diag sd` transfer from a power_capture.py serial log."""
import argparse
from pathlib import Path
import re

def decode(capture):
    data = bytearray()
    offset = end = None
    complete = False
    for line in capture.splitlines():
        match = re.search(r'\[sdlog\] begin path=(\S+) start=(\d+) end=(\d+) encoding=hex', line)
        if match:
            offset, end = int(match[2]), int(match[3])
            data.clear(); complete = False
            continue
        match = re.search(r'\[sdlog\] data ([0-9a-f]{8}) ([0-9a-f]+)$', line)
        if match and offset is not None:
            chunk = bytes.fromhex(match[2])
            if int(match[1], 16) != offset or offset + len(chunk) > end:
                raise ValueError('SD log transfer has a gap, overlap, or invalid size')
            data.extend(chunk); offset += len(chunk)
        match = re.search(r'\[sdlog\] end offset=(\d+) expected=(\d+) complete=(\d+)', line)
        if match and offset is not None:
            complete = int(match[3]) == 1 and int(match[1]) == offset == end == int(match[2])
    if not complete:
        raise ValueError('No complete SD log transfer found; retry diag sd')
    return data

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    try:
        data = decode(args.capture.read_text(errors='replace'))
        with args.output.open('xb') as out:
            out.write(data)
    except (ValueError, OSError) as error:
        parser.exit(1, str(error) + '\n')
    print(f'Decoded {len(data)} SD log bytes to {args.output}')
