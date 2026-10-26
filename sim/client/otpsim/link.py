"""The phone link (docs/ble-link.md) over a SimPhone: framing, Hello,
requests and replies, pushes, uploads and downloads — what the apps'
LinkClient.swift / LinkClient.kt do, for tests."""
import queue
import struct
import threading
import time
import zlib

from .proto import pb

SVC_LINK = "b1c51000-9e0f-4b7a-9c6d-1f2e3a4b5c6d"
CHR_RX = "b1c51001-9e0f-4b7a-9c6d-1f2e3a4b5c6d"
CHR_TX = "b1c51002-9e0f-4b7a-9c6d-1f2e3a4b5c6d"

FIRST, LAST = 0x80, 0x40

ALL_PHONE_FEATURES = None   # filled lazily: every Feature the proto knows


class LinkError(Exception):
    def __init__(self, what, code=None, detail=""):
        super().__init__("%s: %s%s" % (what, code, (" (%s)" % detail) if detail else ""))
        self.code = code
        self.detail = detail


def body_name(msg):
    return msg.WhichOneof("body")


class LinkClient:
    """Attach to a connected (and secured) SimPhone and run the link session.

        phone = SimPhone(dev.ble_port).connect(); phone.secure()
        link = LinkClient(phone).hello()
        settings = link.request(get_settings=pb.GetSettings()).settings
    """

    def __init__(self, phone, platform=None, app_version="sim (1)", features=None):
        self.phone = phone
        self.platform = platform if platform is not None else pb.PLATFORM_IOS
        self.app_version = app_version
        if features is None:
            features = [v.number for v in pb.Feature.values if v.number != 0]
        self.features = features
        self.device_hello = None
        self.device_features = set()
        self._counter = 0
        self._asm = b""
        self._asm_open = False
        self._rx_next = None
        self._req = 0
        self._wlock = threading.Lock()
        self._pending = {}          # req -> queue of DeviceMsg
        self._plock = threading.Lock()
        self.pushes = queue.Queue()   # req 0 messages
        self.push_log = []
        self._push_cbs = []
        self.dropped = 0
        self._xfer = 0

    # -- framing -----------------------------------------------------------------------------

    def _on_tx(self, frag):
        if not frag:
            return
        h = frag[0]
        counter = h & 0x0F
        if self._rx_next is not None and counter != self._rx_next:
            self.dropped += 1
            self._asm, self._asm_open = b"", False
        self._rx_next = (counter + 1) & 0x0F
        if h & FIRST:
            if self._asm_open:
                self.dropped += 1
            self._asm, self._asm_open = b"", True
        elif not self._asm_open:
            self.dropped += 1
            return
        self._asm += frag[1:]
        if h & LAST:
            data, self._asm, self._asm_open = self._asm, b"", False
            msg = pb.DeviceMsg()
            msg.ParseFromString(data)
            self._dispatch(msg)

    def _dispatch(self, msg):
        if msg.req:
            with self._plock:
                q = self._pending.get(msg.req)
            if q is not None:
                q.put(msg)
                return
        self.push_log.append(msg)
        for cb in list(self._push_cbs):
            cb(msg)
        self.pushes.put(msg)

    def send(self, msg, response=None):
        """Encode and write a PhoneMsg, fragmented to the MTU."""
        data = msg.SerializeToString()
        payload = self.phone.att_mtu - 3 - 1
        if response is None:
            response = body_name(msg) == "hello"
        with self._wlock:
            off = 0
            first = True
            while True:
                part = data[off:off + payload]
                off += len(part)
                last = off >= len(data)
                h = (FIRST if first else 0) | (LAST if last else 0) | self._counter
                self._counter = (self._counter + 1) & 0x0F
                self.phone.write(CHR_RX, bytes([h]) + part, response=response)
                first = False
                if last:
                    break

    # -- session -------------------------------------------------------------------------------

    def hello(self, timeout=5.0, unix_ms=None):
        self.phone.subscribe(CHR_TX, callback=self._on_tx)
        h = pb.Hello(proto=pb.PROTO_VERSION_CURRENT, platform=self.platform, app_version=self.app_version,
                     features=self.features, unix_ms=int(unix_ms if unix_ms is not None else time.time() * 1000))
        reply = self.request(hello=h, timeout=timeout)
        if body_name(reply) != "hello":
            raise LinkError("hello", body_name(reply))
        self.device_hello = reply.hello
        self.device_features = set(reply.hello.features)
        return self

    def has(self, feature):
        return feature in self.device_features

    def next_req(self):
        self._req += 1
        return self._req

    def request(self, timeout=5.0, all_pages=False, **body):
        """Send one request and return its first answer (DeviceMsg). With
        all_pages, collect answers until `timeout` passes quietly or a page
        says it is the last (FileList.more == False)."""
        req = self.next_req()
        q = queue.Queue()
        with self._plock:
            self._pending[req] = q
        msg = pb.PhoneMsg(req=req, **body)
        try:
            self.send(msg)
            first = q.get(timeout=timeout)
            if not all_pages:
                return first
            out = [first]
            while self._more(out[-1]):
                out.append(q.get(timeout=timeout))
            return out
        except queue.Empty:
            raise TimeoutError("no answer to %s" % list(body))
        finally:
            with self._plock:
                self._pending.pop(req, None)

    @staticmethod
    def _more(msg):
        b = body_name(msg)
        if b not in ("file_list", "tile_list"):
            return False
        return not getattr(msg, b).last

    def expect_result(self, timeout=5.0, **body):
        r = self.request(timeout=timeout, **body)
        if body_name(r) == "result" and r.result.code != pb.RESULT_OK:
            raise LinkError(list(body)[0], pb.ResultCode.values_by_number[r.result.code].name, r.result.detail)
        return r

    def tell(self, **body):
        """A message that takes no reply (req 0)."""
        self.send(pb.PhoneMsg(req=0, **body), response=False)

    def on_push(self, cb):
        self._push_cbs.append(cb)

    def wait_push(self, kind, predicate=None, timeout=5.0):
        """The next push (req 0) whose body is `kind`, optionally matching."""
        deadline = time.time() + timeout
        while True:
            left = deadline - time.time()
            if left <= 0:
                raise TimeoutError("no %s push" % kind)
            try:
                m = self.pushes.get(timeout=left)
            except queue.Empty:
                continue
            if body_name(m) == kind and (predicate is None or predicate(getattr(m, kind))):
                return getattr(m, kind)

    def drain_pushes(self):
        while True:
            try:
                self.pushes.get_nowait()
            except queue.Empty:
                return

    # -- transfers -----------------------------------------------------------------------------

    def _chunk_room(self):
        return self.phone.att_mtu - 3 - 1 - 18

    def upload(self, target, name, data, timeout=30.0, **extra):
        """UploadBegin → chunks within the window → UploadEnd → TransferDone."""
        self._xfer += 1
        xfer = self._xfer
        req = self.next_req()
        q = queue.Queue()
        with self._plock:
            self._pending[req] = q
        acked = [0]
        acks = threading.Condition()
        done = queue.Queue()

        def watch(m):
            b = body_name(m)
            if b == "transfer_ack" and m.transfer_ack.xfer == xfer:
                with acks:
                    acked[0] = max(acked[0], m.transfer_ack.received)
                    acks.notify_all()
            elif b in ("transfer_done", "transfer_cancel") and getattr(m, b).xfer == xfer:
                done.put(m)
                with acks:
                    acks.notify_all()

        self.on_push(watch)
        try:
            self.send(pb.PhoneMsg(req=req, upload_begin=pb.UploadBegin(
                xfer=xfer, target=target, name=name, size=len(data), crc32=zlib.crc32(data) & 0xFFFFFFFF, **extra)))
            r = q.get(timeout=timeout)
            if body_name(r) == "result":
                raise LinkError("upload " + name, pb.ResultCode.values_by_number[r.result.code].name, r.result.detail)
            window = r.upload_ready.window or 16384

            def forward():   # acks and the end may carry the request's req
                while True:
                    try:
                        m = q.get(timeout=timeout)
                    except queue.Empty:
                        return
                    watch(m)
            threading.Thread(target=forward, daemon=True).start()
            room = self._chunk_room()
            off = 0
            while off < len(data):
                with acks:
                    while off - acked[0] >= window and done.empty():
                        if not acks.wait(timeout):
                            raise TimeoutError("upload stalled at %d" % off)
                if not done.empty():
                    break
                part = data[off:off + room]
                self.send(pb.PhoneMsg(req=0, chunk=pb.Chunk(xfer=xfer, offset=off, data=part)), response=False)
                off += len(part)
            if done.empty():
                end_req = self.next_req()
                with self._plock:
                    self._pending[end_req] = done
                self.send(pb.PhoneMsg(req=end_req, upload_end=pb.UploadEnd(xfer=xfer)))
            m = done.get(timeout=timeout)
            b = body_name(m)
            if b == "transfer_cancel":
                raise LinkError("upload " + name, pb.ResultCode.values_by_number[m.transfer_cancel.reason].name)
            if m.transfer_done.code != pb.RESULT_OK:
                raise LinkError("upload " + name, pb.ResultCode.values_by_number[m.transfer_done.code].name,
                                m.transfer_done.detail)
            return m.transfer_done
        finally:
            self._push_cbs.remove(watch)
            with self._plock:
                self._pending.pop(req, None)

    def download(self, source, name="", window=8192, timeout=30.0, ack_every=2048, have=b""):
        """DownloadRequest → DownloadBegin → chunks (acked) → TransferDone.
        Returns the bytes, CRC-checked. `have`: what the phone already holds
        from an interrupted download — it asks to resume after it, and keeps
        as much of it as DownloadBegin.offset says (0: start over). The
        DownloadBegin is kept in `last_download_begin`."""
        self._xfer += 1
        xfer = self._xfer
        req = self.next_req()
        q = queue.Queue()
        with self._plock:
            self._pending[req] = q
        got = bytearray()
        self.last_download_begin = None
        state = {"acked": 0}
        done = queue.Queue()
        begun = threading.Event()   # `have` is applied before any chunk lands

        def watch(m):
            b = body_name(m)
            if b == "chunk" and m.chunk.xfer == xfer:
                begun.wait(timeout)
                if m.chunk.offset != len(got):
                    done.put(("gap", m.chunk.offset))
                    return
                got.extend(m.chunk.data)
                if len(got) - state["acked"] >= ack_every:
                    state["acked"] = len(got)
                    self.tell(transfer_ack=pb.TransferAck(xfer=xfer, received=len(got)))
            elif b in ("transfer_done", "transfer_cancel") and getattr(m, b).xfer == xfer:
                done.put(m)

        self.on_push(watch)
        try:
            self.send(pb.PhoneMsg(req=req, download=pb.DownloadRequest(xfer=xfer, source=source, name=name,
                                                                         window=window, offset=len(have))))
            r = q.get(timeout=timeout)
            if body_name(r) == "download_begin":
                self.last_download_begin = r.download_begin
                got.extend(have[:r.download_begin.offset])
                state["acked"] = len(got)
            begun.set()
            # Chunks and TransferDone for this transfer may carry the request's req.
            def forward():
                while True:
                    try:
                        m = q.get(timeout=timeout)
                    except queue.Empty:
                        return
                    watch(m)
            threading.Thread(target=forward, daemon=True).start()
            if body_name(r) == "result":
                raise LinkError("download " + name, pb.ResultCode.values_by_number[r.result.code].name,
                                r.result.detail)
            if body_name(r) != "download_begin":
                watch(r)
            m = done.get(timeout=timeout)
            if isinstance(m, tuple):
                raise LinkError("download " + name, "GAP", str(m))
            if body_name(m) == "transfer_cancel":
                raise LinkError("download " + name, pb.ResultCode.values_by_number[m.transfer_cancel.reason].name)
            d = m.transfer_done
            if d.code != pb.RESULT_OK:
                raise LinkError("download " + name, pb.ResultCode.values_by_number[d.code].name, d.detail)
            data = bytes(got)
            if len(data) != d.size or (zlib.crc32(data) & 0xFFFFFFFF) != d.crc32:
                raise LinkError("download " + name, "CRC_MISMATCH",
                                "%d bytes, want %d" % (len(data), d.size))
            return data
        finally:
            self._push_cbs.remove(watch)
            with self._plock:
                self._pending.pop(req, None)
