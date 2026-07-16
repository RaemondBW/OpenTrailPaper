#!/usr/bin/env python3
"""Validate .fit files the way Strava/intervals.icu do: full parse + CRC check.

Reports the messages that decide whether an upload is accepted — a file without
a session/activity is not an activity, however many records it carries.
"""
import sys

import fitdecode


def check(path):
    counts = {}
    try:
        with fitdecode.FitReader(path, check_crc=fitdecode.CrcCheck.RAISE) as fit:
            for frame in fit:
                if frame.frame_type == fitdecode.FIT_FRAME_DATA:
                    counts[frame.name] = counts.get(frame.name, 0) + 1
    except Exception as e:
        print(f"  {path}: REJECTED — {type(e).__name__}: {e}")
        return False

    need = ("file_id", "record", "session", "activity")
    missing = [m for m in need if not counts.get(m)]
    summary = ", ".join(f"{k}={v}" for k, v in sorted(counts.items()))
    if missing:
        print(f"  {path}: REJECTED — no {'/'.join(missing)} ({summary})")
        return False
    print(f"  {path}: OK — {summary}")
    return True


if __name__ == "__main__":
    print("FIT parser validation (CRC enforced):")
    ok = all([check(p) for p in sys.argv[1:]])
    sys.exit(0 if ok else 1)
