"""A phone on the sim's BLE link (simble, sim/PROTOCOL.md).

SimPhone is the radio half of a phone: connect, pair, discover, read, write,
subscribe, notifications, and its own GATT server (the GAP name, AMS). The
link protocol on top is otpsim.link.LinkClient."""
import queue
import socket
import struct
import threading
import time
import uuid as uuidlib

BASE = "-0000-1000-8000-00805f9b34fb"

P_CONNECT, P_DISCOVER, P_READ, P_WRITE, P_SUBSCRIBE, P_SECURE, P_DISCONNECT, P_PASSKEY, P_ADV = (
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x0A)
P_PEER_READ_RSP, P_PEER_NOTIFY, P_PEER_SERVICES = 0x10, 0x11, 0x12
D_CONNECTED, D_DISCOVERED, D_READ_RSP, D_WRITE_RSP, D_SUBSCRIBE_RSP, D_NOTIFY = 0x81, 0x82, 0x83, 0x84, 0x85, 0x86
D_DISCONNECTED, D_PASSKEY_REQ, D_SERVICE_CHANGED, D_SECURITY, D_ADV = 0x87, 0x88, 0x89, 0x8A, 0x8B
D_PEER_READ, D_PEER_SUBSCRIBE, D_PEER_WRITE = 0x90, 0x91, 0x92

ATT_OK, ATT_INSUF_AUTHEN, ATT_INSUF_ENC = 0, 0x05, 0x0F

NOTIFY, WRITE, WRITE_NR, READ = 0x10, 0x08, 0x04, 0x02


def full_uuid(u):
    u = u.lower()
    if len(u) == 4:
        return "0000" + u + BASE
    return u


def ubytes(u):
    return uuidlib.UUID(full_uuid(u)).bytes


def ustr(b):
    return str(uuidlib.UUID(bytes=bytes(b)))


class GattError(Exception):
    def __init__(self, op, status):
        super().__init__("%s failed: ATT 0x%02x" % (op, status))
        self.status = status


class Disconnected(Exception):
    def __init__(self, reason):
        super().__init__("disconnected (HCI 0x%02x)" % reason if reason is not None else "disconnected")
        self.reason = reason


class SimPhone:
    """One phone. `identity` is its identity address — reuse it to be the
    same phone across connections; `bonded` is whether it holds a bond with
    the device (as iOS would after pairing)."""

    def __init__(self, port, identity="d0:00:00:00:00:01", name="Sim iPhone", mtu=247, bonded=False):
        self.port = port
        self.identity = identity
        self.name = name
        self.mtu = mtu
        self.bonded = bonded
        self.sock = None
        self.handle = None
        self.att_mtu = 23
        self.security = None
        self.disconnect_reason = None
        self.connected = False
        self.service_changed = threading.Event()
        self._rsp = queue.Queue()
        self._notify_cbs = {}
        self._notifications = queue.Queue()
        self._security = threading.Event()
        self._passkey_req = threading.Event()
        self._gone = threading.Event()
        self._reader = None
        self._wlock = threading.Lock()
        # This phone's own GATT server: {(svc, chr): bytes}
        self.peer_values = {}
        self.peer_subscriptions = {}

    # -- frames --------------------------------------------------------------------------------

    def _send(self, type_, body=b""):
        f = struct.pack("<HB", len(body) + 1, type_) + body
        with self._wlock:
            self.sock.sendall(f)

    def _read_loop(self):
        buf = b""
        try:
            while True:
                d = self.sock.recv(65536)
                if not d:
                    break
                buf += d
                while len(buf) >= 3:
                    n = struct.unpack_from("<H", buf)[0]
                    if len(buf) < 2 + n:
                        break
                    t, body = buf[2], buf[3:2 + n]
                    buf = buf[2 + n:]
                    self._frame(t, body)
        except OSError:
            pass
        self.connected = False
        self._gone.set()
        self._rsp.put(None)

    def _frame(self, t, b):
        if t == D_NOTIFY:
            u = ustr(b[:16])
            data = bytes(b[16:])
            cb = self._notify_cbs.get(u)
            if cb:
                cb(data)
            else:
                self._notifications.put((u, data))
        elif t == D_SECURITY:
            self.security = dict(encrypted=bool(b[0]), bonded=bool(b[1]), authenticated=bool(b[2]), key_size=b[3])
            if self.security["bonded"] and self.security["encrypted"]:
                self.bonded = True
            self._security.set()
        elif t == D_PASSKEY_REQ:
            self._passkey_req.set()
        elif t == D_SERVICE_CHANGED:
            self.service_changed.set()
        elif t == D_DISCONNECTED:
            self.disconnect_reason = struct.unpack_from("<H", b)[0]
        elif t == D_PEER_READ:
            rid = struct.unpack_from("<I", b)[0]
            key = (ustr(b[4:20]), ustr(b[20:36]))
            val = self.peer_values.get(key)
            body = struct.pack("<IB", rid, 0 if val is not None else 0x0A) + (val or b"")
            self._send(P_PEER_READ_RSP, body)
        elif t == D_PEER_SUBSCRIBE:
            key = (ustr(b[0:16]), ustr(b[16:32]))
            self.peer_subscriptions[key] = struct.unpack_from("<H", b, 32)[0]
        elif t == D_PEER_WRITE:
            pass
        else:
            self._rsp.put((t, bytes(b)))

    def _wait(self, types, timeout=5.0):
        deadline = time.time() + timeout
        while True:
            left = deadline - time.time()
            if left <= 0:
                raise TimeoutError("no response %s" % [hex(t) for t in types])
            try:
                item = self._rsp.get(timeout=left)
            except queue.Empty:
                continue
            if item is None:
                raise Disconnected(self.disconnect_reason)
            if item[0] in types:
                return item
            if item[0] == D_ADV and D_ADV not in types:
                continue

    # -- connection -------------------------------------------------------------------------------

    def connect(self, pair=True, auto_passkey=True, stale_keys=False, timeout=5.0):
        """Connect; `pair`: start pairing if the device asks (a phone without
        a bond). `stale_keys`: the phone's bond holds keys that no longer
        match the device's, so encryption fails. Returns once CONNECTED
        arrives — security follows."""
        self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=timeout)
        self.sock.settimeout(None)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._rsp = queue.Queue()
        self._gone.clear()
        self._security.clear()
        self._passkey_req.clear()
        self.service_changed.clear()
        self.security = None
        self.disconnect_reason = None
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        flags = (1 if self.bonded else 0) | (2 if pair else 0) | (4 if auto_passkey else 0) | (8 if stale_keys else 0)
        ident = bytes(int(x, 16) for x in self.identity.split(":"))
        name = self.name.encode()
        self._send(P_CONNECT, struct.pack("<BHB", 1, self.mtu, flags) + ident + bytes([len(name)]) + name)
        _, b = self._wait([D_CONNECTED], timeout)
        self.handle, self.att_mtu = struct.unpack("<HH", b)
        self.connected = True
        # The phone's own GATT server, as iOS exposes it.
        if self.peer_values:
            body = b"".join(ubytes(s) + ubytes(c) + struct.pack("<I", READ | NOTIFY) for (s, c) in self.peer_values)
            self._send(P_PEER_SERVICES, body)
        return self

    def secure(self, timeout=5.0):
        """Encrypt (or pair) now and wait for the outcome, as a phone does
        when a protected attribute answers 'insufficient authentication'."""
        self._send(P_SECURE)
        return self.wait_security(timeout)

    def wait_security(self, timeout=5.0):
        if not self._security.wait(timeout):
            if self._gone.is_set():
                raise Disconnected(self.disconnect_reason)
            raise TimeoutError("no security outcome")
        return self.security

    def wait_passkey_request(self, timeout=5.0):
        return self._passkey_req.wait(timeout)

    def enter_passkey(self, code):
        self._send(P_PASSKEY, struct.pack("<I", code))

    def disconnect(self):
        if self.sock:
            try:
                self._send(P_DISCONNECT, bytes([0x13]))
            except OSError:
                pass
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.sock.close()
        self._gone.wait(2)
        self.connected = False

    def wait_disconnected(self, timeout=5.0):
        if not self._gone.wait(timeout):
            raise TimeoutError("still connected")
        return self.disconnect_reason

    # -- GATT client ------------------------------------------------------------------------------

    def discover(self):
        self._send(P_DISCOVER)
        _, b = self._wait([D_DISCOVERED])
        out = []
        for i in range(0, len(b), 36):
            out.append((ustr(b[i:i + 16]), ustr(b[i + 16:i + 32]), struct.unpack_from("<I", b, i + 32)[0]))
        return out

    def read(self, chr_uuid):
        self._send(P_READ, ubytes(chr_uuid))
        _, b = self._wait([D_READ_RSP])
        if b[16] != ATT_OK:
            raise GattError("read " + chr_uuid, b[16])
        return bytes(b[17:])

    def write(self, chr_uuid, data, response=True):
        self._send(P_WRITE, ubytes(chr_uuid) + bytes([1 if response else 0]) + bytes(data))
        if response:
            _, b = self._wait([D_WRITE_RSP])
            if b[16] != ATT_OK:
                raise GattError("write " + chr_uuid, b[16])

    def subscribe(self, chr_uuid, callback=None, on=True):
        u = full_uuid(chr_uuid)
        if callback:
            self._notify_cbs[u] = callback
        self._send(P_SUBSCRIBE, ubytes(u) + struct.pack("<H", 1 if on else 0))
        _, b = self._wait([D_SUBSCRIBE_RSP])
        if b[16] != ATT_OK:
            raise GattError("subscribe " + chr_uuid, b[16])

    def notification(self, timeout=5.0):
        """The next notification no callback took: (uuid, bytes)."""
        return self._notifications.get(timeout=timeout)

    @staticmethod
    def scan(port, timeout=3.0):
        """What a scanning phone sees, without connecting: an ADV request on
        a socket that never sends CONNECT. None while not advertising."""
        p = SimPhone(port)
        p.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        p._reader = threading.Thread(target=p._read_loop, daemon=True)
        p._reader.start()
        try:
            adv = p.advertisement()
        finally:
            p.sock.close()
        return adv if adv["advertising"] else None

    def advertisement(self):
        self._send(P_ADV)
        _, b = self._wait([D_ADV])
        i = 0
        adv = b[i]; i += 1
        n = b[i]; i += 1
        name = b[i:i + n].decode(); i += n
        n = b[i]; i += 1
        mfg = bytes(b[i:i + n]); i += n
        return dict(advertising=bool(adv), name=name, mfg=mfg,
                    paired=len(mfg) >= 4 and bool(mfg[3] & 1))

    # -- the phone's own GATT server (AMS) ------------------------------------------------------------

    def set_peer_value(self, svc, chr_uuid, value, notify=True):
        key = (full_uuid(svc), full_uuid(chr_uuid))
        self.peer_values[key] = bytes(value)
        if notify and self.connected and self.peer_subscriptions.get(key):
            self._send(P_PEER_NOTIFY, ubytes(svc) + ubytes(chr_uuid) + bytes(value))
