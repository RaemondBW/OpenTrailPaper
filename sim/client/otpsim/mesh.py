"""Meshtastic on the sim's LoRa air: an independent codec and virtual nodes.

Written from Meshtastic's own definitions (meshtastic/protobufs mesh.proto,
portnums.proto, channel.proto; firmware src/mesh/CryptoEngine.cpp,
RadioInterface.cpp, Channels.cpp, aes-ccm.cpp), NOT from this repo's
src/mesh_proto.cpp, so a test that has the device and a VirtualNode
understand each other is an interoperability check rather than the
firmware's codec agreeing with itself. AES is the operating system's
(CommonCrypto); X25519 is RFC 7748's ladder; SHA-256 is hashlib.

    node = VirtualNode(0x1234abcd, "Alex", "ALX")
    dev.lora_rx(node.text("hi"))                  # the device hears it
    for f in dev.lora_frames():                     # what the device sent
        pkt = node.decode(f)                        # header + decrypted Data

Air relays transmissions between several sims (and virtual nodes) so they
hear each other, with per-link loss and RSSI.
"""
import ctypes
import ctypes.util
import hashlib
import os
import struct
import threading
import time

BROADCAST = 0xFFFFFFFF

# portnums.proto
PORT_TEXT = 1
PORT_POSITION = 3
PORT_NODEINFO = 4
PORT_ROUTING = 5
PORT_TELEMETRY = 67

# mesh.proto Routing.Error
ROUTING_NONE = 0
ROUTING_NO_RESPONSE = 8
ROUTING_PKI_FAILED = 34
ROUTING_PKI_UNKNOWN_PUBKEY = 35

# CryptoEngine / Channels: the well-known default key ("AQ==" = index 1).
DEFAULT_PSK = bytes([0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
                     0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01])

# RadioInterface.cpp modem presets: name -> (sf, bw kHz, cr)
PRESETS = {
    "LongFast": (11, 250.0, 5), "MediumSlow": (10, 250.0, 5), "MediumFast": (9, 250.0, 5),
    "ShortSlow": (8, 250.0, 5), "ShortFast": (7, 250.0, 5), "ShortTurbo": (7, 500.0, 5),
}
PRESET_ORDER = ["LongFast", "MediumSlow", "MediumFast", "ShortSlow", "ShortFast", "ShortTurbo"]
US = (902.0, 928.0)


# ---- crypto ----------------------------------------------------------------------------------

_cc = ctypes.CDLL(ctypes.util.find_library("System") or "/usr/lib/libSystem.dylib")


def aes_ecb(key, block):
    """One AES block (128/256-bit key), by CommonCrypto."""
    out = ctypes.create_string_buffer(len(block))
    moved = ctypes.c_size_t(0)
    # CCCrypt(kCCEncrypt=0, kCCAlgorithmAES=0, kCCOptionECBMode=2, ...)
    st = _cc.CCCrypt(0, 0, 2, key, ctypes.c_size_t(len(key)), None, block, ctypes.c_size_t(len(block)),
                     out, ctypes.c_size_t(len(block)), ctypes.byref(moved))
    if st != 0:
        raise ValueError("CCCrypt %d" % st)
    return out.raw


def aes_ctr(key, nonce16, data):
    """AES-CTR, the 16-byte counter block incremented big-endian (mbedtls)."""
    out = bytearray()
    ctr = int.from_bytes(nonce16, "big")
    for off in range(0, len(data), 16):
        ks = aes_ecb(key, ctr.to_bytes(16, "big"))
        chunk = data[off:off + 16]
        out += bytes(a ^ b for a, b in zip(chunk, ks))
        ctr = (ctr + 1) % (1 << 128)
    return bytes(out)


def channel_nonce(sender, packet_id, extra=0):
    """CryptoEngine::initNonce: packet id as a 64-bit LE, sender, extra nonce."""
    return struct.pack("<QII", packet_id, sender, extra)


def expand_psk(psk):
    """Channels.cpp getKey: 1 byte = well-known key N (0 = none), 16/32 bytes as is."""
    if len(psk) == 0:
        return b""
    if len(psk) == 1:
        if psk[0] == 0:
            return b""
        k = bytearray(DEFAULT_PSK)
        k[15] = (k[15] + psk[0] - 1) & 0xFF
        return bytes(k)
    if len(psk) < 16:   # short keys are zero-padded to AES-128
        return bytes(psk) + bytes(16 - len(psk))
    return bytes(psk)


def xor_hash(b):
    h = 0
    for x in b:
        h ^= x
    return h


def channel_hash(name, psk):
    """Channels::generateHash: xor(name) ^ xor(expanded key); 0 when unencrypted."""
    key = expand_psk(psk)
    if not key:
        return 0
    return xor_hash(name.encode()) ^ xor_hash(key)


def djb2(s):
    h = 5381
    for c in s.encode():
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


def slot_freq(name, bw_khz, start=US[0], end=US[1], slot=None):
    """RadioInterface::applyModemConfig: the channel's frequency (MHz)."""
    n = int((end - start) // (bw_khz / 1000.0))
    if slot is None:
        slot = djb2(name) % n
    return start + bw_khz / 2000.0 + slot * (bw_khz / 1000.0)


def tune_for(preset="LongFast", channel=None):
    """lora_rx tuning for a stock node on `preset`, primary channel `channel`
    (default: unnamed, i.e. the preset's own name)."""
    sf, bw, cr = PRESETS[preset]
    return dict(freq_mhz=slot_freq(channel or preset, bw), bw_khz=bw, sf=sf, cr=cr, sync_word=0x2B)


# X25519, RFC 7748 section 5.
_P = 2 ** 255 - 19
_A24 = 121665


def _clamp(k):
    k = bytearray(k)
    k[0] &= 248
    k[31] &= 127
    k[31] |= 64
    return bytes(k)


def x25519(scalar, u=None):
    k = int.from_bytes(_clamp(scalar), "little")
    u = 9 if u is None else int.from_bytes(u, "little") & ((1 << 255) - 1)
    x1, x2, z2, x3, z3, swap = u, 1, 0, u, 1, 0
    for t in reversed(range(255)):
        kt = (k >> t) & 1
        swap ^= kt
        if swap:
            x2, x3, z2, z3 = x3, x2, z3, z2
        swap = kt
        a, b = x2 + z2, x2 - z2
        aa, bb = a * a % _P, b * b % _P
        e = aa - bb
        c, d = x3 + z3, x3 - z3
        da, cb = d * a % _P, c * b % _P
        x3 = (da + cb) ** 2 % _P
        z3 = x1 * (da - cb) ** 2 % _P
        x2 = aa * bb % _P
        z2 = e * (aa + _A24 * e) % _P
    if swap:
        x2, x3, z2, z3 = x3, x2, z3, z2
    return (x2 * pow(z2, _P - 2, _P) % _P).to_bytes(32, "little")


def pki_key(our_private, their_public):
    """CryptoEngine::setDHPublicKey + hash: SHA-256 of the X25519 secret."""
    return hashlib.sha256(x25519(our_private, their_public)).digest()


def _ccm(key, nonce13, data, tag_len=8, decrypt=False, tag=None):
    """RFC 3610 AES-CCM, L = 2, no associated data (Meshtastic's aes-ccm.cpp)."""
    L = 2

    def a_block(i):
        return bytes([L - 1]) + nonce13 + i.to_bytes(2, "big")

    def ctr(d):
        out = bytearray()
        for n, off in enumerate(range(0, len(d), 16), start=1):
            s = aes_ecb(key, a_block(n))
            out += bytes(x ^ y for x, y in zip(d[off:off + 16], s))
        return bytes(out)

    plain = ctr(data) if decrypt else data
    b0 = bytes([(((tag_len - 2) // 2) << 3) | (L - 1)]) + nonce13 + len(plain).to_bytes(2, "big")
    x = aes_ecb(key, b0)
    for off in range(0, len(plain), 16):
        blk = plain[off:off + 16].ljust(16, b"\0")
        x = aes_ecb(key, bytes(p ^ q for p, q in zip(x, blk)))
    s0 = aes_ecb(key, a_block(0))
    t = bytes(p ^ q for p, q in zip(x[:tag_len], s0))
    if decrypt:
        return plain if t == tag else None
    return ctr(plain), t


def pki_encrypt(key, sender, packet_id, plain, extra=None):
    """CryptoEngine::encryptCurve25519: ciphertext | 8-byte tag | extra nonce."""
    extra = int.from_bytes(os.urandom(4), "little") if extra is None else extra
    # initNonce: the extra nonce overwrites the upper half of the 64-bit id
    nonce = struct.pack("<II", packet_id, extra) + struct.pack("<I", sender) + b"\0\0\0\0"
    ct, tag = _ccm(key, nonce[:13], plain)
    return ct + tag + struct.pack("<I", extra)


def pki_decrypt(key, sender, packet_id, data):
    if len(data) <= 12:
        return None
    ct, tag, extra = data[:-12], data[-12:-4], struct.unpack("<I", data[-4:])[0]
    nonce = struct.pack("<II", packet_id, extra) + struct.pack("<I", sender) + b"\0\0\0\0"
    return _ccm(key, nonce[:13], ct, decrypt=True, tag=tag)


# ---- protobuf (just the wire format) ---------------------------------------------------------

def _varint(v):
    v &= (1 << 64) - 1
    out = bytearray()
    while v >= 0x80:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


def pb_varint(field, v):
    return _varint(field << 3) + _varint(v)


def pb_bytes(field, b):
    if isinstance(b, str):
        b = b.encode()
    return _varint(field << 3 | 2) + _varint(len(b)) + b


def pb_fixed32(field, v):
    return _varint(field << 3 | 5) + struct.pack("<I", v & 0xFFFFFFFF)


def pb_parse(b):
    """{field: [values]}; varints as ints, fixed32 as ints, bytes as bytes."""
    out, i = {}, 0
    while i < len(b):
        t, i = _read_varint(b, i)
        f, w = t >> 3, t & 7
        if w == 0:
            v, i = _read_varint(b, i)
        elif w == 2:
            n, i = _read_varint(b, i)
            v, i = b[i:i + n], i + n
            if i > len(b):
                raise ValueError("truncated")
        elif w == 5:
            v, i = struct.unpack("<I", b[i:i + 4])[0], i + 4
        elif w == 1:
            v, i = struct.unpack("<Q", b[i:i + 8])[0], i + 8
        else:
            raise ValueError("wire type %d" % w)
        out.setdefault(f, []).append(v)
    return out


def _read_varint(b, i):
    v = shift = 0
    while True:
        if i >= len(b):
            raise ValueError("truncated varint")
        c = b[i]
        i += 1
        v |= (c & 0x7F) << shift
        if not c & 0x80:
            return v, i
        shift += 7


def data_msg(portnum, payload, want_response=False, request_id=0, reply_id=0, dest=0, source=0, bitfield=None):
    """mesh.proto Data."""
    b = pb_varint(1, portnum) + (pb_bytes(2, payload) if payload else b"")
    if want_response:
        b += pb_varint(3, 1)
    if dest:
        b += pb_fixed32(4, dest)
    if source:
        b += pb_fixed32(5, source)
    if request_id:
        b += pb_fixed32(6, request_id)
    if reply_id:
        b += pb_fixed32(7, reply_id)
    if bitfield is not None:
        b += pb_varint(9, bitfield)
    return b


def user_msg(node_num, long_name, short_name, public_key=None, hw_model=0):
    """mesh.proto User."""
    b = pb_bytes(1, "!%08x" % node_num) + pb_bytes(2, long_name) + pb_bytes(3, short_name)
    if hw_model:
        b += pb_varint(5, hw_model)
    if public_key:
        b += pb_bytes(8, public_key)
    return b


def position_msg(lat, lon, alt=None, utc=0, sats=0, precision_bits=None):
    """mesh.proto Position: latitude_i/longitude_i are sfixed32 1e-7 degrees."""
    b = pb_fixed32(1, int(round(lat * 1e7))) + pb_fixed32(2, int(round(lon * 1e7)))
    if alt is not None:
        b += pb_varint(3, int(alt))
    if utc:
        b += pb_fixed32(4, utc)
    if sats:
        b += pb_varint(19, sats)
    if precision_bits is not None:
        b += pb_varint(23, precision_bits)
    return b


def routing_msg(error=ROUTING_NONE):
    return pb_varint(3, error)


def header(dest, sender, packet_id, hop_limit=3, hop_start=None, want_ack=False, channel=0, next_hop=0,
           relay=0, via_mqtt=False):
    """The 16-byte radio header (RadioInterface.h PacketHeader)."""
    hop_start = hop_limit if hop_start is None else hop_start
    flags = (hop_limit & 7) | (0x08 if want_ack else 0) | (0x10 if via_mqtt else 0) | ((hop_start & 7) << 5)
    return struct.pack("<IIIBBBB", dest, sender, packet_id, flags, channel, next_hop, relay)


def parse_header(frame):
    dest, sender, pid, flags, ch, nh, relay = struct.unpack("<IIIBBBB", frame[:16])
    return dict(dest=dest, sender=sender, id=pid, hop_limit=flags & 7, want_ack=bool(flags & 8),
                via_mqtt=bool(flags & 0x10), hop_start=flags >> 5, channel=ch, next_hop=nh, relay=relay)


class Channel:
    """A channel as a stock node holds it: name + psk as shared."""

    def __init__(self, name="LongFast", psk=b"\x01"):
        self.name = name
        self.psk = bytes(psk)
        self.key = expand_psk(self.psk)
        self.hash = channel_hash(name, self.psk)

    def encrypt(self, sender, packet_id, plain):
        if not self.key:
            return plain
        return aes_ctr(self.key, channel_nonce(sender, packet_id), plain)

    decrypt = encrypt


class VirtualNode:
    """A stock Meshtastic node, as bytes on the air."""

    def __init__(self, num, long_name="Virtual", short_name="VIRT", channel=None, private_key=None, hop_limit=3):
        self.num = num
        self.long_name = long_name
        self.short_name = short_name
        self.channel = channel or Channel()
        self.private_key = _clamp(private_key or hashlib.sha256(b"vnode%08x" % num).digest())
        self.public_key = x25519(self.private_key)
        self.hop_limit = hop_limit
    @staticmethod
    def new_id():
        """A fresh random packet id (never 0), as a stock node picks them."""
        while True:
            pid = int.from_bytes(os.urandom(4), "little")
            if pid:
                return pid

    def packet(self, data, dest=BROADCAST, packet_id=None, channel=None, want_ack=False, hop_limit=None,
               hop_start=None, pki_to=None, sender=None):
        """A whole frame: header + Data encrypted with the channel key, or, with
        `pki_to` (the recipient's public key), PKI-encrypted (channel byte 0)."""
        ch = channel or self.channel
        pid = packet_id if packet_id is not None else self.new_id()
        hl = self.hop_limit if hop_limit is None else hop_limit
        sender = self.num if sender is None else sender
        if pki_to is not None:
            body = pki_encrypt(pki_key(self.private_key, pki_to), sender, pid, data)
            h = header(dest, sender, pid, hl, hop_start, want_ack, 0)
        else:
            body = ch.encrypt(sender, pid, data)
            h = header(dest, sender, pid, hl, hop_start, want_ack, ch.hash)
        return h + body

    def text(self, text, dest=BROADCAST, **kw):
        return self.packet(data_msg(PORT_TEXT, text.encode() if isinstance(text, str) else text), dest=dest, **kw)

    def nodeinfo(self, dest=BROADCAST, want_response=False, with_key=True, long_name=None, short_name=None,
                 public_key=None, request_id=0, **kw):
        u = user_msg(self.num, self.long_name if long_name is None else long_name,
                     self.short_name if short_name is None else short_name,
                     (public_key or self.public_key) if with_key else None)
        return self.packet(data_msg(PORT_NODEINFO, u, want_response=want_response, request_id=request_id),
                           dest=dest, **kw)

    def position(self, lat, lon, alt=None, utc=0, sats=0, precision_bits=None, dest=BROADCAST, **kw):
        return self.packet(data_msg(PORT_POSITION, position_msg(lat, lon, alt, utc, sats, precision_bits)),
                           dest=dest, **kw)

    def ack(self, dest, request_id, error=ROUTING_NONE, **kw):
        return self.packet(data_msg(PORT_ROUTING, routing_msg(error), request_id=request_id), dest=dest, **kw)

    def decode(self, frame, channels=None, pki_from=None):
        """Header + Data (dict) of a frame, decrypting with our channel(s), or
        PKI when the channel byte is 0 and it is for us (`pki_from`: the
        sender's public key). `data` is None when nothing decrypts it."""
        h = parse_header(frame)
        body = frame[16:]
        h["data"] = None
        h["pki"] = False
        if h["channel"] == 0 and h["dest"] == self.num and pki_from is not None:
            plain = pki_decrypt(pki_key(self.private_key, pki_from), h["sender"], h["id"], body)
            if plain is not None:
                h["data"], h["pki"] = decode_data(plain), True
                return h
        for ch in channels or [self.channel]:
            if ch.hash != h["channel"]:
                continue
            try:
                d = decode_data(ch.decrypt(h["sender"], h["id"], body))
            except ValueError:
                continue
            if d.get("portnum"):
                h["data"] = d
                h["channel_name"] = ch.name
                return h
        return h


def decode_data(b):
    f = pb_parse(b)
    d = dict(portnum=f.get(1, [0])[0], payload=f.get(2, [b""])[0], want_response=bool(f.get(3, [0])[0]),
             dest=f.get(4, [0])[0], source=f.get(5, [0])[0], request_id=f.get(6, [0])[0],
             reply_id=f.get(7, [0])[0], bitfield=f.get(9, [None])[0])
    return d


def decode_user(b):
    f = pb_parse(b)
    s = lambda k: f[k][0].decode() if k in f else ""
    return dict(id=s(1), long_name=s(2), short_name=s(3), hw_model=f.get(5, [0])[0],
                public_key=f.get(8, [None])[0])


def decode_position(b):
    f = pb_parse(b)
    sfix = lambda v: v - (1 << 32) if v & 0x80000000 else v
    out = dict(lat=sfix(f[1][0]) / 1e7 if 1 in f else None, lon=sfix(f[2][0]) / 1e7 if 2 in f else None,
               alt=None, time=f.get(4, [0])[0], sats=f.get(19, [0])[0], precision_bits=f.get(23, [None])[0],
               location_source=f.get(5, [0])[0])
    if 3 in f:
        a = f[3][0]
        out["alt"] = a - (1 << 64) if a & (1 << 63) else a
    return out


def decode_routing(b):
    f = pb_parse(b)
    return f.get(3, [0])[0]


def airtime_ms(nbytes, sf=11, bw_khz=250.0, cr=5, preamble=16):
    """Semtech LoRa time on air (explicit header, CRC on, LDRO from SF11)."""
    import math
    tsym = (2 ** sf) / (bw_khz * 1000.0) * 1000.0
    de = 1 if sf >= 11 else 0
    payload = max(math.ceil((8 * nbytes - 4 * sf + 28 + 16) / (4.0 * (sf - 2 * de))) * cr, 0)
    return (preamble + 4.25 + 8 + payload) * tsym


# ---- the air between several sims -----------------------------------------------------------

class Air:
    """Relays every transmission of each device to the others, as their
    radios would hear it: on the sender's tuning (a receiver tuned elsewhere
    hears nothing), from the moment it starts (so the others' channel-
    activity detection sees it) to the end of its time on air, through each
    receiver's own `lora` impairments. Devices must run at the same clock
    speed. `link(a, b, loss_pct=…, rssi=…, snr=…)` shapes one direction;
    `cut(a, b)` puts them out of range of each other. Listeners (callables
    taking (device, frame dict)) see every transmission, e.g. a VirtualNode's
    test that wants to know what went out.

        with Air([dev1, dev2]) as air:
            ...
    """

    def __init__(self, devices, poll_s=0.02):
        self.devices = list(devices)
        self.poll_s = poll_s
        self.links = {}      # (src, dst) -> dict(rssi, snr, loss_pct) | None (out of range)
        self.listeners = []
        self.log = []        # (device index, frame dict)
        self._seq = {id(d): 0 for d in self.devices}
        self._stop = threading.Event()
        self._thread = None
        self._lock = threading.Lock()
        import random
        self._rng = random.Random(7)

    def link(self, src, dst, **kw):
        self.links[(id(src), id(dst))] = kw
        return self

    def cut(self, a, b):
        self.links[(id(a), id(b))] = None
        self.links[(id(b), id(a))] = None

    def start(self):
        for d in self.devices:
            self._seq[id(d)] = self._last_seq(d)
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return self

    @staticmethod
    def _last_seq(d):
        return d.cmd("lora")["lora"]["tx_seq"]

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(2)

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()

    def pump(self):
        """One pass: relay what each device transmitted since the last."""
        for src in self.devices:
            try:
                r = src.cmd("lora_tx", since=self._seq[id(src)])
            except Exception:
                continue
            self._seq[id(src)] = r["seq"]
            for f in r["frames"]:
                if f.get("aborted"):
                    continue
                with self._lock:
                    self.log.append((self.devices.index(src), f))
                for cb in list(self.listeners):
                    cb(src, f)
                for dst in self.devices:
                    if dst is src:
                        continue
                    shape = self.links.get((id(src), id(dst)), {})
                    if shape is None:
                        continue
                    shape = dict(shape)
                    loss = shape.pop("loss_pct", 0)
                    if loss and self._rng.random() * 100 < loss:
                        continue
                    try:
                        dst.cmd("lora_rx", hex=f["hex"], freq_mhz=f["freq_mhz"], bw_khz=f["bw_khz"], sf=f["sf"],
                                cr=f["cr"], sync_word=f["sync_word"],
                                elapsed_ms=max(0, r["now_ms"] - f["start_ms"]), **shape)
                    except Exception:
                        pass

    def _run(self):
        while not self._stop.is_set():
            self.pump()
            time.sleep(self.poll_s)
