package com.raemond.opentrailpaper.link

import com.raemond.opentrailpaper.link.proto.Chunk
import com.raemond.opentrailpaper.link.proto.DeviceHello
import com.raemond.opentrailpaper.link.proto.DeviceMsg
import com.raemond.opentrailpaper.link.proto.DownloadRequest
import com.raemond.opentrailpaper.link.proto.Feature
import com.raemond.opentrailpaper.link.proto.Hello
import com.raemond.opentrailpaper.link.proto.PhoneMsg
import com.raemond.opentrailpaper.link.proto.ResultCode
import com.raemond.opentrailpaper.link.proto.Source
import com.raemond.opentrailpaper.link.proto.TransferAck
import com.raemond.opentrailpaper.link.proto.TransferCancel
import com.raemond.opentrailpaper.link.proto.UploadBegin
import com.raemond.opentrailpaper.link.proto.UploadEnd
import com.google.protobuf.ByteString
import com.google.protobuf.InvalidProtocolBufferException
import java.io.ByteArrayOutputStream
import java.util.ArrayDeque
import java.util.zip.CRC32

/**
 * Where the link's fragments go: one ATT write each, to the RX characteristic.
 * On Android this is [com.raemond.opentrailpaper.ble.GattQueue], whose
 * completion callback is the flow control (docs/ble-link.md, "Sending
 * priority").
 */
interface LinkTransport {
    /** Largest single write: ATT MTU - 3. */
    val maxWrite: Int

    /**
     * Issue one write. [done] fires once the stack has taken it (or refused
     * it), and only then does the link hand over the next fragment. It may be
     * called before [write] returns.
     */
    fun write(fragment: ByteArray, withoutResponse: Boolean, done: (Boolean) -> Unit)
}

fun interface Cancellable {
    fun cancel()
}

/** Timers, so the tests can run the clock by hand. */
fun interface LinkClock {
    fun schedule(delayMs: Long, block: () -> Unit): Cancellable
}

/**
 * The phone's end of the link (docs/ble-link.md): framing and reassembly,
 * request ids with reply callbacks and timeouts, the two-queue writer (control
 * before bulk), the Hello handshake with its legacy fallback, and the generic
 * upload / download transfers.
 *
 * Confined to ONE thread — BleManager's main thread, where every GATT callback
 * is posted — so nothing here locks. Everything is per connection: [reset]
 * fails what is pending and starts clean.
 */
class LinkClient(
    private val transport: LinkTransport,
    private val clock: LinkClock,
    private val log: (String) -> Unit = {},
) {
    interface Listener {
        /** DeviceHello arrived: the link carries every feature from now on. */
        fun onReady(hello: DeviceHello)

        /** No DeviceHello after every retry: use the legacy protocol. */
        fun onFallback(reason: String)

        /** A push (req 0), or a reply nobody registered a callback for. */
        fun onMessage(msg: DeviceMsg)
    }

    var listener: Listener? = null

    enum class State { IDLE, SUBSCRIBING, HELLO, READY, FALLBACK }

    var state = State.IDLE
        private set

    val ready: Boolean get() = state == State.READY

    var deviceHello: DeviceHello? = null
        private set

    private var features: Set<Feature> = emptySet()

    /** The device listed [f] in its DeviceHello on this connection. */
    fun has(f: Feature): Boolean = ready && f in features

    // MARK: - session

    private var hello: Hello? = null
    private var helloAttempts = 0
    private var helloTimer: Cancellable? = null

    /**
     * The link service is there and its TX subscription has been asked for.
     * [onTxSubscribed] sends Hello at once; without that confirmation it goes
     * after [SUBSCRIBE_WAIT_MS] anyway — a reply proves the subscription works.
     */
    fun begin(h: Hello) {
        reset("new session")
        hello = h
        state = State.SUBSCRIBING
        helloTimer = clock.schedule(SUBSCRIBE_WAIT_MS) {
            if (state == State.SUBSCRIBING) {
                log("hello: no TX subscribe confirmation in ${SUBSCRIBE_WAIT_MS} ms, sending anyway")
                sendHello()
            }
        }
    }

    fun onTxSubscribed() {
        if (state != State.SUBSCRIBING) return
        helloTimer?.cancel()
        sendHello()
    }

    private fun sendHello() {
        val h = hello ?: return
        state = State.HELLO
        helloAttempts += 1
        log("hello: attempt $helloAttempts")
        enqueue(PhoneMsg.newBuilder().setReq(nextReq()).setHello(h).build(), bulk = false)
        helloTimer?.cancel()
        helloTimer = clock.schedule(HELLO_TIMEOUT_MS) {
            if (state != State.HELLO) return@schedule
            if (helloAttempts < HELLO_ATTEMPTS) {
                sendHello()
            } else {
                val why = "no DeviceHello after $helloAttempts Hello(s), ${HELLO_TIMEOUT_MS} ms each"
                log("fallback: $why")
                state = State.FALLBACK
                listener?.onFallback(why)
            }
        }
    }

    private fun onDeviceHello(h: DeviceHello) {
        if (state == State.FALLBACK || state == State.IDLE) {
            log("hello: late DeviceHello ignored (state $state)")
            return
        }
        helloTimer?.cancel(); helloTimer = null
        deviceHello = h
        features = h.featuresList.toSet()
        if (h.maxMessage > 0) reassembler = newReassembler()
        state = State.READY
        log("hello: ready fw=${h.firmware} proto=${h.proto} features=${features.size} " +
            "mtu=${h.mtu} maxMsg=${h.maxMessage} dashPages=${h.dashMaxPages}")
        listener?.onReady(h)
    }

    /**
     * The connection ended (or a new one starts): everything pending fails
     * with RESULT_CANCELLED, every queue empties, every timer stops.
     */
    fun reset(reason: String) {
        if (state != State.IDLE) log("reset: $reason")
        state = State.IDLE
        helloTimer?.cancel(); helloTimer = null
        hello = null
        helloAttempts = 0
        deviceHello = null
        features = emptySet()
        fragmenter.reset()
        reassembler = newReassembler()
        control.clear()
        current = null
        writing = false
        epoch += 1
        val reqs = pending.values.toList()
        pending.clear()
        for (p in reqs) {
            p.timer?.cancel()
            p.onFail(ResultCode.RESULT_CANCELLED, reason)
        }
        val xs = transfers.values.toList()
        transfers.clear()
        for (t in xs) t.abandon(ResultCode.RESULT_CANCELLED, reason)
    }

    // MARK: - receive

    private var reassembler = newReassembler()

    private fun newReassembler() = LinkReassembler(
        maxMessage = 64 * 1024,
        onDrop = { log("rx: dropped partial message ($it)") },
    )

    /** One TX notification. */
    fun onFragment(fragment: ByteArray) {
        if (state == State.IDLE || state == State.FALLBACK) return
        val bytes = reassembler.feed(fragment) ?: return
        val msg = try {
            DeviceMsg.parseFrom(bytes)
        } catch (e: InvalidProtocolBufferException) {
            log("rx: decode failed (${bytes.size} bytes): ${e.message}")
            return
        }
        dispatch(msg)
    }

    private fun dispatch(msg: DeviceMsg) {
        if (msg.hasHello()) {
            pending.remove(msg.req)?.timer?.cancel()
            onDeviceHello(msg.hello)
            return
        }
        if (state != State.READY) {
            log("rx: ${msg.bodyCase} before DeviceHello, ignored")
            return
        }
        // Transfer traffic is addressed by xfer, whatever its req.
        val xfer = xferOf(msg)
        if (xfer != null) {
            pending.remove(msg.req)?.timer?.cancel()   // UploadReady / DownloadBegin answer the begin
            val t = transfers[xfer]
            if (t == null) {
                if (!msg.hasChunk()) log("rx: ${msg.bodyCase} for unknown xfer $xfer")
                return
            }
            t.onMessage(msg)
            return
        }
        val p = if (msg.req != 0) pending[msg.req] else null
        if (p != null) {
            p.timer?.cancel()
            if (p.onReply(msg)) {
                pending.remove(msg.req)
            } else {
                p.timer = armTimeout(p)   // more pages to come
            }
            return
        }
        if (msg.hasResult() && msg.result.code != ResultCode.RESULT_OK) {
            log("rx: req ${msg.req} -> ${msg.result.code} ${msg.result.detail}")
        }
        listener?.onMessage(msg)
    }

    private fun xferOf(m: DeviceMsg): Int? = when {
        m.hasUploadReady() -> m.uploadReady.xfer
        m.hasChunk() -> m.chunk.xfer
        m.hasDownloadBegin() -> m.downloadBegin.xfer
        m.hasTransferAck() -> m.transferAck.xfer
        m.hasTransferCancel() -> m.transferCancel.xfer
        m.hasTransferDone() -> m.transferDone.xfer
        else -> null
    }

    // MARK: - requests

    private class Pending(
        val req: Int,
        val timeoutMs: Long,
        val onReply: (DeviceMsg) -> Boolean,
        val onFail: (ResultCode, String) -> Unit,
    ) {
        var timer: Cancellable? = null
    }

    private val pending = HashMap<Int, Pending>()
    private var lastReq = 0
    private var lastXfer = 0

    private fun nextReq(): Int {
        lastReq = if (lastReq >= 0x3FFF_FFFF) 1 else lastReq + 1
        return lastReq
    }

    private fun nextXfer(): Int {
        lastXfer = if (lastXfer >= 0x7F) 1 else lastXfer + 1   // one-byte varint in every Chunk
        while (transfers.containsKey(lastXfer)) lastXfer = if (lastXfer >= 0x7F) 1 else lastXfer + 1
        return lastXfer
    }

    private fun armTimeout(p: Pending): Cancellable = clock.schedule(p.timeoutMs) {
        if (pending.remove(p.req) != null) {
            log("req ${p.req} timed out after ${p.timeoutMs} ms")
            p.onFail(ResultCode.RESULT_TIMEOUT, "No answer from the device")
        }
    }

    /**
     * Send a request and route the device's answers to [onReply] — every
     * message carrying its req, until [onReply] returns true (lists answer in
     * pages). Each answer re-arms the timeout. [onFail]: no answer in time, or
     * the link went away. Returns the req, or 0 if the link isn't up.
     */
    fun request(
        msg: PhoneMsg.Builder,
        timeoutMs: Long = REQUEST_TIMEOUT_MS,
        onFail: (ResultCode, String) -> Unit = { _, _ -> },
        onReply: (DeviceMsg) -> Boolean,
    ): Int {
        if (!ready) {
            onFail(ResultCode.RESULT_CANCELLED, "Link not ready")
            return 0
        }
        val req = nextReq()
        val built = msg.setReq(req).build()
        if (!fits(built)) {
            onFail(ResultCode.RESULT_TOO_LARGE, "Message too large for the device")
            return 0
        }
        val p = Pending(req, timeoutMs, onReply, onFail)
        pending[req] = p
        p.timer = armTimeout(p)
        enqueue(built, bulk = false)
        return req
    }

    /**
     * Send without a reply callback. With [numbered] the message carries a req
     * (the device answers it), and the answers arrive through
     * [Listener.onMessage]; without, req is 0 (PhoneLocation, acks...).
     */
    fun send(msg: PhoneMsg.Builder, numbered: Boolean = true): Boolean {
        if (!ready) return false
        val built = msg.setReq(if (numbered) nextReq() else 0).build()
        if (!fits(built)) return false
        enqueue(built, bulk = false)
        return true
    }

    private fun fits(m: PhoneMsg): Boolean {
        val max = deviceHello?.maxMessage ?: 0
        if (max > 0 && m.serializedSize > max) {
            log("tx: ${m.bodyCase} is ${m.serializedSize} bytes, device takes $max — not sent")
            return false
        }
        return true
    }

    // MARK: - writer (two queues)

    private class Out(val bytes: ByteArray, val bulk: Boolean, val onSent: (() -> Unit)?)

    private val fragmenter = LinkFragmenter()
    private val control = ArrayDeque<Out>()
    private var current: ArrayDeque<ByteArray>? = null
    private var currentOut: Out? = null
    private var writing = false
    private var pumping = false
    /** Bumped by [reset]: a write callback from an older connection is ignored. */
    private var epoch = 0

    /** The writer's fragment size: our MTU, capped by what the device reported. */
    val maxWrite: Int
        get() {
            val ours = transport.maxWrite
            val theirs = deviceHello?.mtu ?: 0
            return if (theirs > 23) minOf(ours, theirs - 3) else ours
        }

    /** Chunk.data that keeps a whole Chunk message in one fragment. */
    val chunkData: Int
        get() = (maxWrite - 1 - CHUNK_OVERHEAD).coerceIn(MIN_CHUNK, MAX_CHUNK)

    private fun enqueue(m: PhoneMsg, bulk: Boolean, onSent: (() -> Unit)? = null) {
        control.addLast(Out(m.toByteArray(), bulk, onSent))
        pump()
    }

    /**
     * Next message: control first, else a chunk from a running upload. A
     * message's fragments go back-to-back; priority is only between messages.
     */
    private fun pump() {
        if (pumping) return
        pumping = true
        try {
            while (!writing) {
                var q = current
                if (q == null || q.isEmpty()) {
                    val out = control.pollFirst() ?: nextBulk() ?: break
                    q = ArrayDeque(fragmenter.fragment(out.bytes, maxWrite))
                    current = q
                    currentOut = out
                }
                val frag = q.removeFirst()
                val out = currentOut!!
                val last = q.isEmpty()
                val e = epoch
                writing = true
                transport.write(frag, out.bulk) { ok ->
                    if (e == epoch) {
                        writing = false
                        if (!ok) {
                            log("tx: write failed, dropping the rest of a ${out.bytes.size}-byte message")
                            current = null
                        } else if (last) {
                            out.onSent?.invoke()
                        }
                        pump()
                    }
                }
            }
        } finally {
            pumping = false
        }
    }

    private var bulkCursor = 0

    private fun nextBulk(): Out? {
        val ups = transfers.values.filterIsInstance<Upload>()
        if (ups.isEmpty()) return null
        for (i in ups.indices) {
            val u = ups[(bulkCursor + i) % ups.size]
            val out = u.nextChunk() ?: continue
            bulkCursor = (bulkCursor + i + 1) % ups.size
            return out
        }
        return null
    }

    // MARK: - transfers

    private val transfers = LinkedHashMap<Int, Transfer>()

    private abstract inner class Transfer(override val xfer: Int) : Handle {
        abstract fun onMessage(m: DeviceMsg)

        /** Ended locally (reset, timeout): report it, send nothing. */
        abstract fun abandon(code: ResultCode, detail: String)

        /** The phone gives up: tell the device and report it locally. */
        override fun cancel() {
            if (transfers.remove(xfer) == null) return
            log("transfer $xfer: cancelled by the app")
            sendCancel(ResultCode.RESULT_CANCELLED)
            abandon(ResultCode.RESULT_CANCELLED, "Cancelled")
        }

        fun sendCancel(reason: ResultCode) {
            if (ready) {
                send(
                    PhoneMsg.newBuilder().setTransferCancel(
                        TransferCancel.newBuilder().setXfer(xfer).setReason(reason),
                    ),
                    numbered = false,
                )
            }
        }
    }

    /** Progress and outcome of an upload. Every method runs on the link's thread. */
    interface UploadListener {
        /** Bytes handed to the radio (sent) and confirmed by the device (acked). */
        fun onProgress(sent: Long, acked: Long, total: Long) {}

        /** Everything is sent; the device is checking and saving (it may wait for the SD card). */
        fun onWaiting() {}

        /** Terminal, exactly once: TransferDone, a refusal, a cancel or a lost link. */
        fun onDone(code: ResultCode, detail: String)
    }

    interface DownloadListener {
        fun onBegin(size: Long, offset: Long) {}
        fun onProgress(received: Long, total: Long) {}

        /** The whole file (including any resumed prefix), CRC checked. */
        fun onDone(data: ByteArray)

        /** Terminal failure. [partial]: the bytes held so far, for a later resume. */
        fun onFail(code: ResultCode, detail: String, partial: ByteArray?)
    }

    /** A running transfer, for cancelling it. */
    interface Handle {
        val xfer: Int
        fun cancel()
    }

    /**
     * Upload [data]. [begin] carries the target and its fields (name, version,
     * art format...); xfer, size and crc32 are filled in here. Returns null
     * (after [UploadListener.onDone]) when the link is not up.
     */
    fun upload(begin: UploadBegin.Builder, data: ByteArray, listener: UploadListener): Handle? {
        if (!ready) {
            listener.onDone(ResultCode.RESULT_CANCELLED, "Link not ready")
            return null
        }
        val u = Upload(nextXfer(), data, listener)
        transfers[u.xfer] = u
        val crc = CRC32().apply { update(data) }.value.toInt()
        begin.setXfer(u.xfer).setSize(data.size).setCrc32(crc)
        log("upload ${u.xfer}: begin ${begin.target} \"${begin.name}\" ${data.size} B")
        val req = request(
            PhoneMsg.newBuilder().setUploadBegin(begin),
            onFail = { code, detail -> u.failed(code, detail, sendCancel = false) },
        ) { m ->
            // UploadReady is routed by xfer; what lands here is the refusal.
            if (m.hasResult()) u.failed(m.result.code, m.result.detail.ifEmpty { "Refused" }, sendCancel = false)
            true
        }
        if (req == 0) u.failed(ResultCode.RESULT_TOO_LARGE, "Message too large", sendCancel = false)
        return u
    }

    private inner class Upload(
        xfer: Int,
        private val data: ByteArray,
        private val listener: UploadListener,
    ) : Transfer(xfer) {
        private var window = 0
        private var sent = 0
        private var acked = 0
        private var sending = false
        private var ended = false
        private var stall: Cancellable? = null
        private var waitingTimer: Cancellable? = null

        override fun onMessage(m: DeviceMsg) {
            when {
                m.hasUploadReady() -> {
                    window = m.uploadReady.window.takeIf { it > 0 } ?: DEFAULT_UPLOAD_WINDOW
                    sending = true
                    armStall()
                    if (data.isEmpty()) end()
                    pump()
                }
                m.hasTransferAck() -> {
                    acked = maxOf(acked, m.transferAck.received.coerceAtMost(data.size))
                    armStall()
                    listener.onProgress(sent.toLong(), acked.toLong(), data.size.toLong())
                    pump()
                }
                m.hasTransferCancel() -> failed(
                    m.transferCancel.reason, "Cancelled by the device (${m.transferCancel.reason})",
                    sendCancel = false,
                )
                m.hasTransferDone() -> {
                    transfers.remove(xfer)
                    stopTimers()
                    val d = m.transferDone
                    log("upload $xfer: done ${d.code} ${d.detail}")
                    if (d.code == ResultCode.RESULT_OK) {
                        listener.onProgress(data.size.toLong(), data.size.toLong(), data.size.toLong())
                    }
                    listener.onDone(d.code, d.detail)
                }
            }
        }

        fun nextChunk(): Out? {
            if (!sending || ended || sent >= data.size) return null
            val n = minOf(chunkData, data.size - sent)
            if (sent + n - acked > window) return null       // wait for an ack
            val offset = sent
            sent += n
            val msg = PhoneMsg.newBuilder().setChunk(
                Chunk.newBuilder().setXfer(xfer).setOffset(offset)
                    .setData(ByteString.copyFrom(data, offset, n)),
            ).build()
            val sentNow = sent
            if (sent >= data.size) end()
            return Out(msg.toByteArray(), bulk = true) {
                listener.onProgress(sentNow.toLong(), acked.toLong(), data.size.toLong())
            }
        }

        /** All bytes are queued: UploadEnd goes next on the control queue, so
         *  it can never pass a chunk. */
        private fun end() {
            ended = true
            stall?.cancel(); stall = null
            control.addLast(
                Out(PhoneMsg.newBuilder().setUploadEnd(UploadEnd.newBuilder().setXfer(xfer)).build().toByteArray(),
                    bulk = false, onSent = null),
            )
            // TransferDone may wait for the SD card (a ride recording); no
            // timeout from here, but say so if it takes a while.
            waitingTimer = clock.schedule(WAITING_NOTICE_MS) {
                if (transfers[xfer] === this) listener.onWaiting()
            }
        }

        private fun armStall() {
            stall?.cancel()
            if (ended) return
            stall = clock.schedule(UPLOAD_STALL_MS) {
                if (transfers[xfer] === this && !ended) failed(ResultCode.RESULT_TIMEOUT, "The device stopped answering", sendCancel = true)
            }
        }

        private fun stopTimers() {
            stall?.cancel(); stall = null
            waitingTimer?.cancel(); waitingTimer = null
        }

        fun failed(code: ResultCode, detail: String, sendCancel: Boolean) {
            if (transfers.remove(xfer) == null) return
            if (sendCancel) sendCancel(code)
            abandon(code, detail)
        }

        override fun abandon(code: ResultCode, detail: String) {
            stopTimers()
            log("upload $xfer: failed $code $detail")
            listener.onDone(code, detail)
        }
    }

    /**
     * Download [name] from [source]. [prefix]: bytes already held from an
     * interrupted attempt; the device resumes after them (or restarts at 0 if
     * it can't), and the CRC check covers the whole file either way.
     */
    fun download(
        source: Source,
        name: String,
        listener: DownloadListener,
        prefix: ByteArray? = null,
        window: Int = 0,
    ): Handle? {
        if (!ready) {
            listener.onFail(ResultCode.RESULT_CANCELLED, "Link not ready", prefix)
            return null
        }
        val d = Download(nextXfer(), prefix ?: ByteArray(0), listener)
        transfers[d.xfer] = d
        log("download ${d.xfer}: $source \"$name\" from ${d.offset}")
        val req = request(
            PhoneMsg.newBuilder().setDownload(
                DownloadRequest.newBuilder().setXfer(d.xfer).setSource(source)
                    .setName(LinkText.cut(name, MAX_NAME_BYTES)).setOffset(d.offset).setWindow(window),
            ),
            onFail = { code, detail -> d.failed(code, detail, sendCancel = false) },
        ) { m ->
            if (m.hasResult()) d.failed(m.result.code, m.result.detail.ifEmpty { "Refused" }, sendCancel = false)
            true
        }
        if (req == 0) d.failed(ResultCode.RESULT_TOO_LARGE, "Message too large", sendCancel = false)
        return d
    }

    private inner class Download(
        xfer: Int,
        prefix: ByteArray,
        private val listener: DownloadListener,
    ) : Transfer(xfer) {
        val offset = prefix.size
        private var buf = ByteArrayOutputStream(maxOf(1024, prefix.size)).apply { write(prefix) }
        private var crc = CRC32().apply { update(prefix) }
        private var size = -1L
        private var lastAck = 0
        private var idle: Cancellable? = null

        override fun onMessage(m: DeviceMsg) {
            when {
                m.hasDownloadBegin() -> {
                    val b = m.downloadBegin
                    if (b.offset != buf.size()) {
                        if (b.offset == 0) {
                            // The device can't resume: start over.
                            buf = ByteArrayOutputStream(maxOf(1024, b.size))
                            crc = CRC32()
                        } else {
                            failed(ResultCode.RESULT_BAD_REQUEST, "Device resumed at ${b.offset}, not ${buf.size()}", true)
                            return
                        }
                    }
                    size = b.size.toLong() and 0xFFFF_FFFFL
                    lastAck = buf.size()
                    armIdle()
                    listener.onBegin(size, b.offset.toLong())
                    listener.onProgress(buf.size().toLong(), size)
                }
                m.hasChunk() -> {
                    val c = m.chunk
                    if (size < 0) return                      // before DownloadBegin: not ours yet
                    if (c.offset != buf.size()) {
                        failed(ResultCode.RESULT_BAD_REQUEST, "Chunk at ${c.offset}, expected ${buf.size()}", true)
                        return
                    }
                    val bytes = c.data.toByteArray()
                    buf.write(bytes)
                    crc.update(bytes)
                    armIdle()
                    val got = buf.size()
                    if (got - lastAck >= ACK_EVERY || got.toLong() >= size) {
                        lastAck = got
                        send(
                            PhoneMsg.newBuilder().setTransferAck(
                                TransferAck.newBuilder().setXfer(xfer).setReceived(got),
                            ),
                            numbered = false,
                        )
                    }
                    listener.onProgress(got.toLong(), size)
                }
                m.hasTransferCancel() -> failed(
                    m.transferCancel.reason, "Cancelled by the device (${m.transferCancel.reason})", false,
                )
                m.hasTransferDone() -> {
                    transfers.remove(xfer)
                    idle?.cancel(); idle = null
                    val d = m.transferDone
                    val bytes = buf.toByteArray()
                    when {
                        d.code != ResultCode.RESULT_OK -> report(d.code, d.detail.ifEmpty { "Failed" }, bytes)
                        bytes.size.toLong() != (d.size.toLong() and 0xFFFF_FFFFL) ->
                            report(ResultCode.RESULT_IO_ERROR, "Got ${bytes.size} of ${d.size} bytes", bytes)
                        crc.value.toInt() != d.crc32 -> {
                            log("download $xfer: CRC mismatch")
                            report(ResultCode.RESULT_CRC_MISMATCH, "Checksum mismatch", null)
                        }
                        else -> {
                            log("download $xfer: done ${bytes.size} B")
                            listener.onDone(bytes)
                        }
                    }
                }
            }
        }

        private fun armIdle() {
            idle?.cancel()
            idle = clock.schedule(DOWNLOAD_IDLE_MS) {
                if (transfers[xfer] === this) failed(ResultCode.RESULT_TIMEOUT, "The device stopped sending", true)
            }
        }

        fun failed(code: ResultCode, detail: String, sendCancel: Boolean) {
            if (transfers.remove(xfer) == null) return
            if (sendCancel) sendCancel(code)
            abandon(code, detail)
        }

        override fun abandon(code: ResultCode, detail: String) {
            idle?.cancel(); idle = null
            report(code, detail, buf.toByteArray())
        }

        private fun report(code: ResultCode, detail: String, partial: ByteArray?) {
            log("download $xfer: failed $code $detail")
            listener.onFail(code, detail, partial)
        }
    }

    companion object {
        const val SUBSCRIBE_WAIT_MS = 2_000L
        const val HELLO_TIMEOUT_MS = 3_000L
        const val HELLO_ATTEMPTS = 3
        const val REQUEST_TIMEOUT_MS = 10_000L

        /** Envelope + Chunk fields around Chunk.data (docs/ble-link.md). */
        const val CHUNK_OVERHEAD = 16
        private const val MIN_CHUNK = 16
        /** link.options Chunk.data max_size. */
        const val MAX_CHUNK = 512
        const val DEFAULT_UPLOAD_WINDOW = 16 * 1024
        /** The device cancels after 20 s without data; give its cancel time to arrive. */
        const val UPLOAD_STALL_MS = 25_000L
        const val WAITING_NOTICE_MS = 4_000L
        const val ACK_EVERY = 2048
        const val DOWNLOAD_IDLE_MS = 20_000L
        /** link.options UploadBegin.name / DownloadRequest.name, less the NUL. */
        const val MAX_NAME_BYTES = 63
    }
}
