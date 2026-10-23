package com.raemond.opentrailpaper

import com.google.protobuf.ByteString
import com.raemond.opentrailpaper.link.Cancellable
import com.raemond.opentrailpaper.link.LinkClient
import com.raemond.opentrailpaper.link.LinkClock
import com.raemond.opentrailpaper.link.LinkFragmenter
import com.raemond.opentrailpaper.link.LinkReassembler
import com.raemond.opentrailpaper.link.LinkTransport
import com.raemond.opentrailpaper.link.proto.Chunk
import com.raemond.opentrailpaper.link.proto.DeviceHello
import com.raemond.opentrailpaper.link.proto.DeviceMsg
import com.raemond.opentrailpaper.link.proto.DownloadBegin
import com.raemond.opentrailpaper.link.proto.Feature
import com.raemond.opentrailpaper.link.proto.FileList
import com.raemond.opentrailpaper.link.proto.GetSettings
import com.raemond.opentrailpaper.link.proto.Hello
import com.raemond.opentrailpaper.link.proto.ListKind
import com.raemond.opentrailpaper.link.proto.ListRequest
import com.raemond.opentrailpaper.link.proto.PhoneMsg
import com.raemond.opentrailpaper.link.proto.Result
import com.raemond.opentrailpaper.link.proto.ResultCode
import com.raemond.opentrailpaper.link.proto.Settings
import com.raemond.opentrailpaper.link.proto.Source
import com.raemond.opentrailpaper.link.proto.Target
import com.raemond.opentrailpaper.link.proto.TransferAck
import com.raemond.opentrailpaper.link.proto.TransferDone
import com.raemond.opentrailpaper.link.proto.UploadBegin
import com.raemond.opentrailpaper.link.proto.UploadReady
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.util.zip.CRC32

/**
 * The link client against a fake transport and a hand-run clock: the Hello
 * handshake and its fallback, requests, the writer's priority, and the
 * upload / download state machines.
 */
class LinkClientTest {

    /** Hand-run timers. */
    private class FakeClock : LinkClock {
        var now = 0L
        private val timers = mutableListOf<Triple<Long, () -> Unit, BooleanArray>>()
        override fun schedule(delayMs: Long, block: () -> Unit): Cancellable {
            val live = booleanArrayOf(true)
            timers += Triple(now + delayMs, block, live)
            return Cancellable { live[0] = false }
        }

        fun advance(ms: Long) {
            val until = now + ms
            while (true) {
                val next = timers.filter { it.third[0] && it.first <= until }.minByOrNull { it.first } ?: break
                timers.remove(next)
                now = next.first
                next.second()
            }
            now = until
        }
    }

    /**
     * The radio plus the device's half of the framing: writes complete when
     * [autoComplete] (or by hand), phone messages are reassembled and decoded.
     */
    private inner class FakeDevice : LinkTransport {
        override var maxWrite = 244
        var autoComplete = true
        val writes = mutableListOf<Pair<ByteArray, Boolean>>()
        private val held = ArrayDeque<(Boolean) -> Unit>()
        private val rx = LinkReassembler(onDrop = { throw AssertionError("device dropped: $it") })
        private val tx = LinkFragmenter()
        val received = mutableListOf<PhoneMsg>()

        override fun write(fragment: ByteArray, withoutResponse: Boolean, done: (Boolean) -> Unit) {
            writes += fragment to withoutResponse
            assertTrue("fragment over MTU", fragment.size <= maxWrite)
            rx.feed(fragment)?.let { received += PhoneMsg.parseFrom(it) }
            if (autoComplete) done(true) else held.addLast(done)
        }

        fun completeOne() = held.removeFirst()(true)
        fun completeAll() { while (held.isNotEmpty()) completeOne() }

        fun send(m: DeviceMsg) {
            for (f in tx.fragment(m.toByteArray(), maxWrite)) client.onFragment(f)
        }

        fun take(): List<PhoneMsg> = received.toList().also { received.clear() }
    }

    private lateinit var clock: FakeClock
    private lateinit var dev: FakeDevice
    private lateinit var client: LinkClient
    private val events = mutableListOf<String>()
    private val pushes = mutableListOf<DeviceMsg>()

    @Before fun setUp() {
        clock = FakeClock()
        dev = FakeDevice()
        client = LinkClient(dev, clock) { /* println("link: $it") */ }
        client.listener = object : LinkClient.Listener {
            override fun onReady(hello: DeviceHello) { events += "ready ${hello.firmware}" }
            override fun onFallback(reason: String) { events += "fallback" }
            override fun onMessage(msg: DeviceMsg) { pushes += msg }
        }
    }

    private val hello = Hello.newBuilder().setProto(1).setAppVersion("test").build()

    private fun deviceHello(req: Int) = DeviceMsg.newBuilder().setReq(req).setHello(
        DeviceHello.newBuilder().setFirmware("v9.99").setMaxMessage(4096).setMtu(247)
            .addFeatures(Feature.FEATURE_SETTINGS).addFeatures(Feature.FEATURE_RIDES),
    ).build()

    private fun connect() {
        client.begin(hello)
        client.onTxSubscribed()
        val h = dev.take().single()
        assertTrue(h.hasHello())
        dev.send(deviceHello(h.req))
        assertTrue(client.ready)
    }

    // MARK: - session

    @Test fun helloWaitsForTheSubscriptionThenGoes() {
        client.begin(hello)
        assertTrue(dev.take().isEmpty())
        client.onTxSubscribed()
        val m = dev.take().single()
        assertTrue(m.hasHello())
        assertTrue(m.req != 0)
        dev.send(deviceHello(m.req))
        assertEquals(listOf("ready v9.99"), events)
        assertTrue(client.has(Feature.FEATURE_RIDES))
        assertFalse(client.has(Feature.FEATURE_MESH))
    }

    @Test fun helloGoesAfterTwoSecondsWithoutConfirmation() {
        client.begin(hello)
        clock.advance(1_999)
        assertTrue(dev.take().isEmpty())
        clock.advance(1)
        assertTrue(dev.take().single().hasHello())
    }

    @Test fun threeHellosThenLegacyFallback() {
        client.begin(hello)
        client.onTxSubscribed()
        clock.advance(3_000)
        clock.advance(3_000)
        assertEquals(3, dev.take().count { it.hasHello() })
        assertTrue(events.isEmpty())
        clock.advance(3_000)
        assertEquals(listOf("fallback"), events)
        assertEquals(LinkClient.State.FALLBACK, client.state)
        // A late DeviceHello still switches to the link: the device now pushes only there.
        dev.send(deviceHello(1))
        assertTrue(client.ready)
        assertEquals(listOf("fallback", "ready v9.99"), events)
    }

    @Test fun answerToARetriedHelloIsAccepted() {
        client.begin(hello)
        client.onTxSubscribed()
        clock.advance(3_000)
        val reqs = dev.take().map { it.req }
        assertEquals(2, reqs.size)
        dev.send(deviceHello(reqs[0]))   // the device answers the first one late
        assertTrue(client.ready)
        clock.advance(10_000)
        assertTrue("no fallback after ready", "fallback" !in events)
    }

    // MARK: - requests

    @Test fun requestGetsItsReplyAndOthersArePushes() {
        connect()
        var reply: DeviceMsg? = null
        client.request(PhoneMsg.newBuilder().setGetSettings(GetSettings.getDefaultInstance())) { reply = it; true }
        val req = dev.take().single().req
        dev.send(DeviceMsg.newBuilder().setReq(0).setSettings(Settings.newBuilder().setFtpW(100)).build())
        assertNull(reply)
        assertEquals(1, pushes.size)
        dev.send(DeviceMsg.newBuilder().setReq(req).setSettings(Settings.newBuilder().setFtpW(250)).build())
        assertEquals(250, reply!!.settings.ftpW)
    }

    @Test fun pagedListAndTimeoutRearm() {
        connect()
        val names = mutableListOf<String>()
        var failed: ResultCode? = null
        client.request(
            PhoneMsg.newBuilder().setList(ListRequest.newBuilder().setKind(ListKind.LIST_RIDES)),
            onFail = { c, _ -> failed = c },
        ) { m ->
            names += m.fileList.filesList.map { it.name }
            m.fileList.last
        }
        val req = dev.take().single().req
        fun page(n: String, last: Boolean) = DeviceMsg.newBuilder().setReq(req).setFileList(
            FileList.newBuilder().setKind(ListKind.LIST_RIDES).setLast(last)
                .addFiles(com.raemond.opentrailpaper.link.proto.FileEntry.newBuilder().setName(n)),
        ).build()
        clock.advance(9_000)
        dev.send(page("a.fit", false))
        clock.advance(9_000)                      // re-armed by the page: still alive
        dev.send(page("b.fit", true))
        clock.advance(20_000)
        assertEquals(listOf("a.fit", "b.fit"), names)
        assertNull(failed)
    }

    @Test fun requestTimesOut() {
        connect()
        var failed: ResultCode? = null
        client.request(PhoneMsg.newBuilder().setGetSettings(GetSettings.getDefaultInstance()), onFail = { c, _ -> failed = c }) { true }
        clock.advance(LinkClient.REQUEST_TIMEOUT_MS)
        assertEquals(ResultCode.RESULT_TIMEOUT, failed)
    }

    @Test fun resetFailsPendingAndStopsTimers() {
        connect()
        var failed: ResultCode? = null
        client.request(PhoneMsg.newBuilder().setGetSettings(GetSettings.getDefaultInstance()), onFail = { c, _ -> failed = c }) { true }
        client.reset("disconnected")
        assertEquals(ResultCode.RESULT_CANCELLED, failed)
        assertFalse(client.ready)
        failed = null
        clock.advance(60_000)
        assertNull("timer cleared", failed)
    }

    @Test fun largeMessagesFragmentAndStayInOrder() {
        dev.maxWrite = 23
        connect()
        val text = "x".repeat(1500)
        client.send(PhoneMsg.newBuilder().setSetDash(com.raemond.opentrailpaper.link.proto.DashConfig.newBuilder().setText(text)))
        client.send(PhoneMsg.newBuilder().setGetSettings(GetSettings.getDefaultInstance()))
        val got = dev.take()
        assertEquals(text, got[0].setDash.text)
        assertTrue(got[1].hasGetSettings())
    }

    // MARK: - upload

    private class UpRec : LinkClient.UploadListener {
        var done: ResultCode? = null
        var progress = 0L
        var waiting = false
        override fun onProgress(sent: Long, acked: Long, total: Long) { progress = maxOf(progress, sent) }
        override fun onWaiting() { waiting = true }
        override fun onDone(code: ResultCode, detail: String) { done = code }
    }

    @Test fun uploadWindowAcksAndDone() {
        connect()
        val data = ByteArray(10_000) { (it * 7).toByte() }
        val rec = UpRec()
        client.upload(UploadBegin.newBuilder().setTarget(Target.TARGET_ROUTE).setName("r.gpx"), data, rec)
        val begin = dev.take().single()
        val ub = begin.uploadBegin
        assertEquals(10_000, ub.size)
        assertEquals(CRC32().apply { update(data) }.value.toInt(), ub.crc32)
        assertEquals("r.gpx", ub.name)

        dev.send(DeviceMsg.newBuilder().setReq(begin.req).setUploadReady(UploadReady.newBuilder().setXfer(ub.xfer).setWindow(4096)).build())
        var chunks = dev.take()
        assertTrue(chunks.all { it.hasChunk() })
        val got = ByteArrayOutputStream()
        for (c in chunks) { assertEquals(got.size(), c.chunk.offset); c.chunk.data.writeTo(got) }
        assertTrue("window respected", got.size() <= 4096)
        // Every chunk is a single fragment, sent without response.
        assertEquals(chunks.size, dev.writes.count { it.second })

        while (got.size() < data.size) {
            dev.send(DeviceMsg.newBuilder().setTransferAck(TransferAck.newBuilder().setXfer(ub.xfer).setReceived(got.size())).build())
            chunks = dev.take()
            for (c in chunks) {
                if (c.hasChunk()) { assertEquals(got.size(), c.chunk.offset); c.chunk.data.writeTo(got) }
            }
        }
        assertArrayEquals(data, got.toByteArray())
        // UploadEnd went out after the last chunk.
        val last = chunks.last()
        assertTrue(last.hasUploadEnd())
        clock.advance(LinkClient.WAITING_NOTICE_MS)
        assertTrue(rec.waiting)
        assertNull(rec.done)
        dev.send(DeviceMsg.newBuilder().setTransferDone(TransferDone.newBuilder().setXfer(ub.xfer).setCode(ResultCode.RESULT_OK)).build())
        assertEquals(ResultCode.RESULT_OK, rec.done)
        assertEquals(10_000L, rec.progress)
    }

    @Test fun controlMessagesJumpTheChunkQueue() {
        connect()
        dev.autoComplete = false
        val rec = UpRec()
        client.upload(UploadBegin.newBuilder().setTarget(Target.TARGET_MAP), ByteArray(5_000), rec)
        dev.completeAll()
        val begin = dev.take().single()
        dev.send(DeviceMsg.newBuilder().setUploadReady(UploadReady.newBuilder().setXfer(begin.uploadBegin.xfer).setWindow(16_384)).build())
        // One chunk is on the radio; a control message queued now goes next.
        assertEquals(1, dev.take().count { it.hasChunk() })
        client.send(PhoneMsg.newBuilder().setGetSettings(GetSettings.getDefaultInstance()))
        dev.completeOne()
        val next = dev.take().single()
        assertTrue(next.hasGetSettings())
        dev.autoComplete = true
        dev.completeAll()
        assertTrue(dev.take().all { it.hasChunk() || it.hasUploadEnd() })
    }

    @Test fun uploadRefused() {
        connect()
        val rec = UpRec()
        client.upload(UploadBegin.newBuilder().setTarget(Target.TARGET_FIRMWARE), ByteArray(100), rec)
        val begin = dev.take().single()
        dev.send(DeviceMsg.newBuilder().setReq(begin.req).setResult(Result.newBuilder().setCode(ResultCode.RESULT_BUSY)).build())
        assertEquals(ResultCode.RESULT_BUSY, rec.done)
        clock.advance(60_000)
        assertTrue(dev.take().isEmpty())
    }

    @Test fun uploadCancelledByDisconnect() {
        connect()
        val rec = UpRec()
        client.upload(UploadBegin.newBuilder().setTarget(Target.TARGET_FIRMWARE), ByteArray(100), rec)
        client.reset("disconnected")
        assertEquals(ResultCode.RESULT_CANCELLED, rec.done)
    }

    // MARK: - download

    private class DownRec : LinkClient.DownloadListener {
        var data: ByteArray? = null
        var fail: ResultCode? = null
        var partial: ByteArray? = null
        var progress = 0L
        override fun onProgress(received: Long, total: Long) { progress = received }
        override fun onDone(data: ByteArray) { this.data = data }
        override fun onFail(code: ResultCode, detail: String, partial: ByteArray?) { fail = code; this.partial = partial }
    }

    /** The device streams [file] from [from], [chunk] bytes a message, acking checked. */
    private fun serve(req: PhoneMsg, file: ByteArray, from: Int = req.download.offset, chunk: Int = 200, crc: Int? = null) {
        val x = req.download.xfer
        dev.send(DeviceMsg.newBuilder().setReq(req.req).setDownloadBegin(
            DownloadBegin.newBuilder().setXfer(x).setSize(file.size).setOffset(from)).build())
        var off = from
        var lastAck = from
        while (off < file.size) {
            val n = minOf(chunk, file.size - off)
            dev.send(DeviceMsg.newBuilder().setChunk(Chunk.newBuilder().setXfer(x).setOffset(off)
                .setData(ByteString.copyFrom(file, off, n))).build())
            off += n
            for (m in dev.take()) {
                assertTrue(m.hasTransferAck())
                assertTrue(m.transferAck.received > lastAck)
                lastAck = m.transferAck.received
            }
            assertTrue("acked at least every 2 KB", off - lastAck < LinkClient.ACK_EVERY)
        }
        dev.send(DeviceMsg.newBuilder().setTransferDone(TransferDone.newBuilder().setXfer(x)
            .setCode(ResultCode.RESULT_OK).setSize(file.size)
            .setCrc32(crc ?: CRC32().apply { update(file) }.value.toInt())).build())
    }

    private val file = ByteArray(9_000) { (it * 13 + 5).toByte() }

    @Test fun downloadWithAcksAndCrc() {
        connect()
        val rec = DownRec()
        client.download(Source.SOURCE_RIDE, "a.fit", rec)
        val req = dev.take().single()
        assertEquals("a.fit", req.download.name)
        assertEquals(0, req.download.offset)
        serve(req, file)
        assertArrayEquals(file, rec.data)
        assertEquals(9_000L, rec.progress)
    }

    @Test fun downloadCrcMismatchFails() {
        connect()
        val rec = DownRec()
        client.download(Source.SOURCE_RIDE, "a.fit", rec)
        serve(dev.take().single(), file, crc = 1234)
        assertNull(rec.data)
        assertEquals(ResultCode.RESULT_CRC_MISMATCH, rec.fail)
    }

    @Test fun downloadResumesAndChecksTheWholeFile() {
        connect()
        val rec = DownRec()
        client.download(Source.SOURCE_RIDE, "a.fit", rec, prefix = file.copyOf(4_000))
        val req = dev.take().single()
        assertEquals(4_000, req.download.offset)
        serve(req, file)
        assertArrayEquals(file, rec.data)
    }

    @Test fun downloadRestartsWhenTheDeviceCannotResume() {
        connect()
        val rec = DownRec()
        client.download(Source.SOURCE_RIDE, "a.fit", rec, prefix = ByteArray(4_000))   // wrong bytes
        serve(dev.take().single(), file, from = 0)
        assertArrayEquals(file, rec.data)
    }

    @Test fun downloadRefusedAndInterrupted() {
        connect()
        val refused = DownRec()
        client.download(Source.SOURCE_RIDE, "a.fit", refused)
        val r1 = dev.take().single()
        dev.send(DeviceMsg.newBuilder().setReq(r1.req).setResult(Result.newBuilder().setCode(ResultCode.RESULT_RECORDING)).build())
        assertEquals(ResultCode.RESULT_RECORDING, refused.fail)

        val cut = DownRec()
        client.download(Source.SOURCE_LOG_TODAY, "", cut)
        val r2 = dev.take().single()
        val x = r2.download.xfer
        dev.send(DeviceMsg.newBuilder().setReq(r2.req).setDownloadBegin(DownloadBegin.newBuilder().setXfer(x).setSize(9_000)).build())
        dev.send(DeviceMsg.newBuilder().setChunk(Chunk.newBuilder().setXfer(x).setOffset(0).setData(ByteString.copyFrom(file, 0, 500))).build())
        client.reset("disconnected")
        assertEquals(ResultCode.RESULT_CANCELLED, cut.fail)
        assertNotNull(cut.partial)
        assertArrayEquals(file.copyOf(500), cut.partial)
    }

    @Test fun downloadBeginWithoutReqEchoStillAnswersTheRequest() {
        connect()
        val rec = DownRec()
        client.download(Source.SOURCE_RIDE, "a.fit", rec)
        val r = dev.take().single()
        val x = r.download.xfer
        dev.send(DeviceMsg.newBuilder().setDownloadBegin(DownloadBegin.newBuilder().setXfer(x).setSize(300)).build())
        clock.advance(LinkClient.REQUEST_TIMEOUT_MS + 1)   // the request must not time out under it
        dev.send(DeviceMsg.newBuilder().setChunk(Chunk.newBuilder().setXfer(x).setOffset(0).setData(ByteString.copyFrom(file, 0, 300))).build())
        dev.send(DeviceMsg.newBuilder().setTransferDone(TransferDone.newBuilder().setXfer(x).setCode(ResultCode.RESULT_OK)
            .setSize(300).setCrc32(CRC32().apply { update(file, 0, 300) }.value.toInt())).build())
        assertNull(rec.fail)
        assertArrayEquals(file.copyOf(300), rec.data)
    }

    @Test fun downloadIdleTimeout() {
        connect()
        val rec = DownRec()
        client.download(Source.SOURCE_RIDE, "a.fit", rec)
        val r = dev.take().single()
        dev.send(DeviceMsg.newBuilder().setReq(r.req).setDownloadBegin(DownloadBegin.newBuilder().setXfer(r.download.xfer).setSize(100)).build())
        clock.advance(LinkClient.DOWNLOAD_IDLE_MS)
        assertEquals(ResultCode.RESULT_TIMEOUT, rec.fail)
        assertTrue("cancel sent", dev.take().single().hasTransferCancel())
    }
}
