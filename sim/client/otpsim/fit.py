"""A small FIT reader for what the firmware's FitWriter writes (src/fit_writer.cpp):
definition and data messages with normal headers, little- or big-endian
fields, developer data skipped. Enough to read a ride back field by field.

    msgs = fit.read(data)                       # [(global msg number, {field: value})]
    recs = fit.records(data)                    # record messages, decoded
    ses = fit.session(data)                     # the session summary
"""
import struct

RECORD, LAP, SESSION, EVENT, ACTIVITY = 20, 19, 18, 21, 34

# base type -> (struct code, size, invalid value)
_BASE = {
    0x00: ("B", 1, 0xFF), 0x01: ("b", 1, 0x7F), 0x02: ("B", 1, 0xFF),
    0x83: ("h", 2, 0x7FFF), 0x84: ("H", 2, 0xFFFF), 0x85: ("i", 4, 0x7FFFFFFF),
    0x86: ("I", 4, 0xFFFFFFFF), 0x07: ("s", 1, None), 0x88: ("f", 4, None),
    0x89: ("d", 8, None), 0x0A: ("B", 1, 0x00), 0x8B: ("H", 2, 0x0000),
    0x8C: ("I", 4, 0x00000000), 0x0D: ("B", 1, 0xFF), 0x8E: ("q", 8, 0x7FFFFFFFFFFFFFFF),
    0x8F: ("Q", 8, 0xFFFFFFFFFFFFFFFF), 0x90: ("Q", 8, 0),
}


class FitError(Exception):
    pass


def read(data):
    """Every data message, in order: (global message number, {field number:
    value}). An invalid value (the base type's 'no data') reads as None."""
    if len(data) < 12 or data[8:12] != b".FIT":
        raise FitError("not a FIT file")
    hdr = data[0]
    size = struct.unpack_from("<I", data, 4)[0]
    pos, end = hdr, hdr + size
    if end > len(data):
        raise FitError("data size %d runs past the file (%d bytes)" % (size, len(data)))
    defs = {}
    out = []
    while pos < end:
        h = data[pos]
        pos += 1
        if h & 0x80:
            raise FitError("compressed timestamp header at %d: not written by FitWriter" % (pos - 1))
        local = h & 0x0F
        if h & 0x40:   # definition
            arch = data[pos + 1]
            endian = ">" if arch else "<"
            gmsg = struct.unpack_from(endian + "H", data, pos + 2)[0]
            n = data[pos + 4]
            pos += 5
            fields = []
            for _ in range(n):
                fnum, fsize, btype = data[pos], data[pos + 1], data[pos + 2]
                fields.append((fnum, fsize, btype))
                pos += 3
            dev = []
            if h & 0x20:
                nd = data[pos]
                pos += 1
                dev = [data[pos + 3 * i + 1] for i in range(nd)]
                pos += 3 * nd
            defs[local] = (gmsg, endian, fields, sum(dev))
        else:
            if local not in defs:
                raise FitError("data message for undefined local type %d at %d" % (local, pos - 1))
            gmsg, endian, fields, devsize = defs[local]
            vals = {}
            for fnum, fsize, btype in fields:
                raw = data[pos:pos + fsize]
                pos += fsize
                code, bsize, invalid = _BASE.get(btype, ("B", 1, None))
                if code == "s":
                    vals[fnum] = raw.split(b"\0")[0].decode("utf-8", "replace")
                elif fsize == bsize:
                    v = struct.unpack(endian + code, raw)[0]
                    vals[fnum] = None if invalid is not None and v == invalid else v
                else:
                    vals[fnum] = bytes(raw)
            pos += devsize
            out.append((gmsg, vals))
    return out


# Record fields (FIT profile) with the firmware's scaling undone.
def records(data):
    """Record messages: dicts with timestamp, lat/lon (degrees), altitude_m,
    distance_m, speed_ms, power_w, hr_bpm, cadence_rpm (None = not recorded)."""
    out = []
    for g, v in read(data):
        if g != RECORD:
            continue
        lat, lon = v.get(0), v.get(1)
        out.append(dict(
            timestamp=v.get(253),
            lat=None if lat is None else lat * 180.0 / 2 ** 31,
            lon=None if lon is None else lon * 180.0 / 2 ** 31,
            altitude_m=None if v.get(2) is None else v[2] / 5.0 - 500,
            distance_m=None if v.get(5) is None else v[5] / 100.0,
            speed_ms=None if v.get(6) is None else v[6] / 1000.0,
            power_w=v.get(7), hr_bpm=v.get(3), cadence_rpm=v.get(4)))
    return out


def session(data):
    """The (last) session message: avg_speed_ms, avg_hr_bpm, avg_power_w,
    normalized_power_w, total_distance_m, timer_s, elapsed_s."""
    ses = [v for g, v in read(data) if g == SESSION]
    if not ses:
        raise FitError("no session message")
    v = ses[-1]
    scale = lambda k, d: None if v.get(k) is None else v[k] / d
    return dict(avg_speed_ms=scale(14, 1000.0), avg_hr_bpm=v.get(16), avg_power_w=v.get(20),
                normalized_power_w=v.get(34), total_distance_m=scale(9, 100.0),
                timer_s=scale(8, 1000.0), elapsed_s=scale(7, 1000.0))
