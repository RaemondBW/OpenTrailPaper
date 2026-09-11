#!/usr/bin/env python3
"""Validate one offline Memfault SDK export and prepare files for Memfault tools.

Accepts an SD memfault-*.log or a completed `memfault export` serial capture.
Checks chunk offsets/length/CRC and coredump framing; performs no upload.
Format sources are the pinned SDK's memfault_chunk_transport.c,
memfault_data_packetizer.c and memfault_coredump.c.
"""
import argparse
import base64
import binascii
import hashlib
import json
from pathlib import Path
import re
import struct


def varint(data, pos):
    value = 0
    for shift in range(0, 35, 7):
        if pos >= len(data):
            raise ValueError('Truncated chunk varint')
        byte = data[pos]; pos += 1
        value |= (byte & 127) << shift
        if not byte & 128:
            if value > 0xffffffff:
                raise ValueError('Chunk varint overflow')
            return value, pos
    raise ValueError('Invalid chunk varint')


def decode(text):
    encoded = re.findall(r'MC:([A-Za-z0-9+/=]+):', text)
    if not encoded:
        raise ValueError('No Memfault SDK chunks found')
    message = bytearray(); expected = None; complete = False
    for i, item in enumerate(encoded):
        chunk = base64.b64decode(item, validate=True)
        if not chunk or complete:
            raise ValueError('Expected exactly one complete export')
        header = chunk[0]; more = bool(header & 0x40); continuation = bool(header & 0x80)
        if header & 0x3f != (0 if continuation else 8):
            raise ValueError('Unsupported Memfault chunk configuration')
        pos = 1
        if continuation:
            if not i:
                raise ValueError('Export starts with a continuation; retry from beginning')
            offset, pos = varint(chunk, pos)
            if offset != len(message):
                raise ValueError('Chunk gap, overlap, or out-of-order data')
        else:
            if i:
                raise ValueError('New message before export completed')
            if more:
                expected, pos = varint(chunk, pos)
        if not more:
            if len(chunk) - pos < 2:
                raise ValueError('Missing chunk CRC')
            payload = chunk[pos:-2]
        else:
            payload = chunk[pos:]
        message.extend(payload)
        if expected is not None and len(message) > expected:
            raise ValueError('Export exceeds advertised length')
        if not more:
            if expected is not None and len(message) != expected:
                raise ValueError('Export length mismatch')
            if binascii.crc_hqx(message, 0) != int.from_bytes(chunk[-2:], 'little'):
                raise ValueError('Memfault chunk CRC mismatch')
            complete = True
    if not complete:
        raise ValueError('Incomplete export; flash is retained, retry memfault export')
    if not message or message[0] != 1:
        raise ValueError('Expected an uncompressed, key-free Memfault coredump')
    core = bytes(message[1:])
    manifest = inspect_core(core)
    manifest.update(chunks=len(encoded), chunk_crc_verified=True,
                    coredump_sha256=hashlib.sha256(core).hexdigest())
    return ''.join('MC:' + item + ':\n' for item in encoded), core, manifest


def inspect_core(core):
    if len(core) < 28:
        raise ValueError('Truncated coredump')
    magic, version, size = struct.unpack_from('<III', core)
    if magic != 0x45524f43 or version != 2 or size != len(core):
        raise ValueError('Invalid Memfault v2 coredump header/length')
    footer, flags, _, _ = struct.unpack_from('<IIII', core, len(core) - 16)
    if footer != 0x504d5544:
        raise ValueError('Invalid Memfault coredump footer')
    result = dict(format='Memfault coredump v2', bytes=size,
                  memory_capture_truncated=bool(flags & 1), registers=[], memory_regions=[])
    fields = {2:'device_serial', 4:'hardware_version', 10:'software_version', 11:'software_type'}
    pos = 12
    while pos < len(core) - 16:
        if pos + 12 > len(core) - 16:
            raise ValueError('Truncated coredump block header')
        kind, address, length = struct.unpack_from('<III', core, pos)
        pos += 12
        if pos + length > len(core) - 16:
            raise ValueError('Truncated coredump block body')
        data = core[pos:pos+length]; pos += length
        kind &= 255
        if kind in fields:
            result[fields[kind]] = data.rstrip(b'\0').decode('utf-8', errors='replace')
        elif kind == 12:
            result['build_id'] = data.hex()
        elif kind == 5 and length == 4:
            result['trace_reason'] = hex(int.from_bytes(data, 'little'))
        elif kind == 7 and length == 4:
            result['machine_type'] = int.from_bytes(data, 'little')
        elif kind == 0 and length == 100:
            regs = struct.unpack('<25I', data)
            result['registers'].append(dict(core=len(result['registers']),
                collection_type=regs[0], pc=hex(regs[1]), sp=hex(regs[4]),
                exception_cause=regs[23], exception_address=hex(regs[24])))
        elif kind == 1:
            result['memory_regions'].append(dict(address=hex(address), bytes=length))
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture',type=Path)
    parser.add_argument('output',type=Path,help='New directory; existing outputs are never replaced')
    args=parser.parse_args()
    try:
        chunks,core,manifest=decode(args.capture.read_text(errors='replace'))
        args.output.mkdir(parents=True,exist_ok=False)
        (args.output/'chunks.log').write_text(chunks)
        (args.output/'coredump.mflt').write_bytes(core)
        (args.output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    except (ValueError,OSError,binascii.Error) as error:
        parser.exit(1,str(error)+'\n')
    print(json.dumps(manifest,indent=2))

if __name__=='__main__':
    main()
