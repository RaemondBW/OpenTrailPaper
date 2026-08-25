#!/usr/bin/env python3
"""Phase-2 bridge: serves web/emulator/ and pipes its WebSocket to QEMU.

    tools/emu/run-qemu.sh          # terminal 1: the machine
    tools/emu/serve.py             # terminal 2: this; then open the URL
    tools/emu/serve.py --launch    # or let this spawn QEMU itself

One WebSocket at /ws, one channel byte per binary message: channel 0 is the
device wire — frames OUT come from UART1 (tcp:5556); events IN go to the
firmware's DRAM mailbox through QEMU's gdbstub (tcp:3333), because the
esp32s3 machine model delivers no UART RX at all (see epd_compat_emu.cpp).
Dependency-free on purpose: the WebSocket half is ~60 lines of RFC6455 and
the gdb client ~40 lines of remote-serial-protocol — less to install than any
package, and all this needs.
"""
import argparse
import base64
import hashlib
import http.server
import os
import socket
import struct
import subprocess
import sys
import threading

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
WEB = os.path.join(ROOT, "web", "emulator")
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
FRAMES_PORT = 5556          # QEMU's second -serial: UART1 TX, the frame stream
GDB_PORT = 3333             # QEMU's gdbstub: our way INTO the guest
ELF = os.path.join(ROOT, ".pio", "build", "t5s3-emu", "firmware.elf")
NM = os.path.expanduser(
    "~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-nm")
MAILBOX_SIZE = 4096
_mailbox_addr = None


def mailbox_addr():
    """Where g_emuMailbox lives — from the ELF, once."""
    global _mailbox_addr
    if _mailbox_addr is None:
        out = subprocess.check_output([NM, ELF]).decode()
        for line in out.splitlines():
            if line.endswith(" g_emuMailbox"):
                _mailbox_addr = int(line.split()[0], 16)
                break
        else:
            raise SystemExit("[serve] g_emuMailbox not in firmware.elf — "
                             "is this the t5s3-emu build?")
    return _mailbox_addr


class GdbMailbox:
    """Writes into the firmware's event ring over QEMU's gdb remote protocol.

    Layout mirrors EmuMailbox in emu_input.h:
    [magic u32][head u32][tail u32][buf[4096]].

    Connecting a gdb client HALTS the esp32s3 CPU and it stays halted for as
    long as the connection is open, so a persistent attach would freeze the
    guest (no frames). Each push() therefore CONNECTS (halts), writes the ring,
    and DETACHES (resumes) — a sub-millisecond stall the eye never sees, and
    between events the guest runs free.
    """

    def __init__(self):
        self.base = mailbox_addr()
        with self._session() as g:      # one probe to fail fast on a bad build
            if self._read_u32(g, self.base) != 0x4F54504D:
                raise RuntimeError("mailbox magic mismatch")

    class _session:
        def __enter__(self):
            self.g = socket.create_connection(("127.0.0.1", GDB_PORT), timeout=5)
            self.g.settimeout(5)
            return self.g

        def __exit__(self, *exc):
            try:
                self.g.sendall(b"$D#44")   # detach -> resume the CPU
                self.g.recv(64)
            except OSError:
                pass
            self.g.close()

    @staticmethod
    def _txn(g, body: str) -> str:
        ck = sum(body.encode()) & 0xFF
        g.sendall(f"${body}#{ck:02x}".encode())
        buf = b""
        while True:
            buf += g.recv(4096)
            i = buf.find(b"$")
            j = buf.find(b"#", i)
            if i >= 0 and j >= 0 and len(buf) >= j + 3:
                g.sendall(b"+")
                return buf[i + 1:j].decode()

    def _read_u32(self, g, addr):
        return int.from_bytes(bytes.fromhex(self._txn(g, f"m{addr:x},4")), "little")

    def push(self, data: bytes):
        with self._session() as g:
            head = self._read_u32(g, self.base + 4)
            tail = self._read_u32(g, self.base + 8)
            free = MAILBOX_SIZE - ((head - tail) & 0xFFFFFFFF)
            data = data[:max(0, free - 1)]
            if not data:
                return
            off = head & (MAILBOX_SIZE - 1)
            first = min(len(data), MAILBOX_SIZE - off)
            self._txn(g, f"M{self.base + 12 + off:x},{first:x}:{data[:first].hex()}")
            if first < len(data):
                rest = data[first:]
                self._txn(g, f"M{self.base + 12:x},{len(rest):x}:{rest.hex()}")
            head = (head + len(data)) & 0xFFFFFFFF
            self._txn(g, f"M{self.base + 4:x},4:{head.to_bytes(4, 'little').hex()}")


def ws_send(sock, payload: bytes):
    hdr = bytearray([0x82])   # FIN + binary
    n = len(payload)
    if n < 126:
        hdr.append(n)
    elif n < 65536:
        hdr.append(126)
        hdr += struct.pack(">H", n)
    else:
        hdr.append(127)
        hdr += struct.pack(">Q", n)
    sock.sendall(bytes(hdr) + payload)


def ws_recv(sock):
    """One frame; None on close. Client frames are masked per the RFC."""
    def need(n):
        buf = b""
        while len(buf) < n:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError
            buf += chunk
        return buf

    b1, b2 = need(2)
    op = b1 & 0x0F
    n = b2 & 0x7F
    if n == 126:
        n = struct.unpack(">H", need(2))[0]
    elif n == 127:
        n = struct.unpack(">Q", need(8))[0]
    mask = need(4) if b2 & 0x80 else None
    data = need(n) if n else b""
    if mask:
        data = bytes(c ^ mask[i % 4] for i, c in enumerate(data))
    if op == 0x8:
        return None
    if op == 0x9:   # ping -> pong
        sock.sendall(b"\x8a" + bytes([len(data)]) + data)
        return b""
    return data


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=WEB, **kw)

    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path != "/ws":
            return super().do_GET()
        key = self.headers.get("Sec-WebSocket-Key", "")
        accept = base64.b64encode(
            hashlib.sha1((key + GUID).encode()).digest()).decode()
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept)
        self.end_headers()
        self.bridge(self.connection)

    def bridge(self, ws):
        try:
            frames = socket.create_connection(("127.0.0.1", FRAMES_PORT), timeout=3)
            frames.settimeout(None)
        except OSError:
            print(f"[serve] frame port tcp:{FRAMES_PORT} not up — is run-qemu.sh running?")
            return
        try:
            mailbox = GdbMailbox()
        except (OSError, RuntimeError, subprocess.CalledProcessError) as e:
            print(f"[serve] gdb mailbox unavailable ({e}) — input disabled")
            mailbox = None
        stop = threading.Event()

        def pump():
            try:
                while not stop.is_set():
                    data = frames.recv(4096)
                    if not data:
                        break
                    ws_send(ws, b"\x00" + data)
            except OSError:
                pass
            stop.set()

        threading.Thread(target=pump, daemon=True).start()
        try:
            while not stop.is_set():
                msg = ws_recv(ws)
                if msg is None:
                    break
                if len(msg) >= 2 and msg[0] == 0 and mailbox:
                    mailbox.push(msg[1:])
        except (ConnectionError, OSError):
            pass
        stop.set()
        frames.close()
        if mailbox:
            mailbox.sock.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--launch", action="store_true",
                    help="spawn tools/emu/run-qemu.sh too")
    args = ap.parse_args()

    if args.launch:
        subprocess.Popen([os.path.join(ROOT, "tools", "emu", "run-qemu.sh")],
                         stdout=sys.stdout, stderr=sys.stderr)

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"[serve] http://localhost:{args.port}/  (ws bridge on /ws)")
    srv.serve_forever()


if __name__ == "__main__":
    main()
