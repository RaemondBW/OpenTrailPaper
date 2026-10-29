package com.raemond.opentrailpaper.sim

import android.Manifest
import android.app.Application
import android.content.Context
import android.os.Looper
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.ble.BleManager.ConnState
import com.raemond.opentrailpaper.ble.MeshMessage
import com.raemond.opentrailpaper.ble.MeshNode
import com.raemond.opentrailpaper.ble.SimRadio
import com.raemond.opentrailpaper.data.Prefs
import com.raemond.opentrailpaper.link.proto.Feature
import com.raemond.opentrailpaper.mesh.MeshChannelUrl
import com.raemond.opentrailpaper.sim.MeshAir.Channel
import com.raemond.opentrailpaper.sim.MeshAir.VirtualNode
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Ignore
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import java.time.Duration
import java.util.Base64

/**
 * The Mesh screen's data — channels, neighbours, the conversation, sending —
 * end to end: the app's real BleManager on SimRadio, the real firmware in
 * otp-sim with its SX1262 stand-in (`--lora`), and stock Meshtastic nodes
 * that exist only as frames on the air ([MeshAir.VirtualNode], put there with
 * control `lora_rx`, and what the device transmits read back with `lora_tx`
 * and decrypted here). The codec in MeshAir follows Meshtastic's definitions,
 * not the firmware's, so these are interoperability checks too.
 *
 * Everything asserted on the app side is BleManager state the Compose screens
 * render (meshChannels, meshNodes, meshMessages, meshState, meshStats).
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = SimTestApp::class)
class MeshSimTest {

    private lateinit var app: Application
    private lateinit var sim: FirmwareSim
    private var radio: SimRadio? = null
    private var ble: BleManager? = null
    private val log = StringBuilder()

    /** The public channel every stock node is on out of the box. */
    private val longFast = Channel()

    /** What `lora_tx` has been read up to. */
    private var txSeq = 0L

    @Before
    fun setUp() {
        if (System.getenv("OTP_SIM_REQUIRED") != null) {
            check(FirmwareSim.binary != null) { "OTP_SIM_REQUIRED, but sim/build/otp-sim is not built" }
        }
        assumeTrue("sim/build/otp-sim is not built (run sim/build.sh)", FirmwareSim.binary != null)
        app = RuntimeEnvironment.getApplication()
        shadowOf(app).grantPermissions(Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.BLUETOOTH_SCAN)
        Prefs.onboarded = true
        sim = FirmwareSim.start(args = listOf("--lora", "--mac", MAC))
    }

    @After
    fun tearDown() {
        if (::sim.isInitialized) println(dump())
        runCatching { radio?.setEnabled(false) }
        if (::sim.isInitialized) sim.close()
    }

    // ---- harness -----------------------------------------------------------------------------

    /** Pair, Hello, and the Mesh screen's first refresh. */
    private fun connect(): BleManager {
        val store = app.getSharedPreferences("simradio-mesh", Context.MODE_PRIVATE)
        val r = SimRadio(
            "127.0.0.1", sim.blePort, store,
            SimRadio.Options(
                phoneName = "Sim mesh",
                directConnectTimeoutMs = 1_500,
                log = { synchronized(log) { log.append(System.currentTimeMillis() % 100_000).append(' ').append(it).append('\n') } },
            ),
        )
        radio = r
        val ble = BleManager(app, r)
        this.ble = ble
        waitFor("pairing and Hello", 30_000) { connected(ble) }
        assumeTrue("firmware without the mesh", ble.has(Feature.FEATURE_MESH))
        waitFor("the device's mesh state", 10_000) { !ble.meshState.isUnknown && ble.meshChannels.isNotEmpty() }
        return ble
    }

    private fun connected(ble: BleManager) = ble.state == ConnState.CONNECTED && ble.linkReady && ble.deviceHello != null

    /**
     * The Mesh screen's radio switch (mesh is off on a fresh device). Waits
     * out the NodeInfo the device announces itself with 5 s after coming up,
     * so frames put on the air afterwards are never lost to its half-duplex
     * radio being busy transmitting.
     */
    private fun meshOn(ble: BleManager) {
        assertFalse("mesh is off on a fresh device", ble.meshState.enabled)
        ble.setMeshEnabled(true)
        waitForMeshState(ble, "the radio up") { it.enabled && it.radioOk }
        val hello = deviceSends("the device's NodeInfo announcement", 15_000, listOf(longFast)) {
            it.port == MeshAir.PORT_NODEINFO && it.dest == MeshAir.BROADCAST
        }
        assertEquals("the device announces its own node number", ble.meshState.nodeNum, hello.sender)
        waitForRadioListening()
    }

    /**
     * The device's mesh state once it satisfies [cond], asking again each
     * second as reopening the Mesh screen would: the radio switch's own
     * follow-up never comes (see radioSwitchShowsTheRadioOnWithoutReopeningTheScreen).
     */
    private fun waitForMeshState(ble: BleManager, what: String, cond: (com.raemond.opentrailpaper.ble.MeshState) -> Boolean) {
        var asked = System.currentTimeMillis()
        waitFor(what, 10_000) {
            if (System.currentTimeMillis() - asked > 1_000) { ble.refreshMesh(); asked = System.currentTimeMillis() }
            cond(ble.meshState)
        }
    }

    /** The device's radio is back to receiving, nothing left to send. */
    private fun waitForRadioListening() {
        waitFor("the device's radio listening", 10_000) {
            val l = sim.lora()
            l.get("mode").asString == "rx" && l.get("in_flight").asInt == 0
        }
        runFor(300)
    }

    /** [frame] on the air for the device; waits for its time on air, so frames arrive in order. */
    private fun hear(frame: ByteArray, rssi: Int = -80, snr: Int = 8) {
        val r = sim.loraRx(frame, rssi, snr)
        assertFalse("the frame was lost on the air: $r", r.get("lost").asBoolean)
    }

    /**
     * The next frame the device transmits that [match]es (decrypted with
     * [channels]); everything it sent on the way is skipped.
     */
    private fun deviceSends(
        what: String, timeoutMs: Long, channels: List<Channel>, match: (MeshAir.Decoded) -> Boolean,
    ): MeshAir.Decoded {
        var found: MeshAir.Decoded? = null
        val seen = mutableListOf<String>()
        waitFor("the device to send $what (sent so far: $seen)", timeoutMs) {
            val (frames, seq) = sim.loraTx(txSeq)
            for (f in frames) {
                txSeq = f.seq
                val d = MeshAir.decode(f.bytes, channels)
                seen += d.toString()
                if (found == null && match(d)) found = d
                if (found != null) break
            }
            if (found == null) txSeq = seq
            found != null
        }
        return found!!
    }

    /** Every frame the device transmitted since the last look. */
    private fun sentSince(channels: List<Channel>): List<MeshAir.Decoded> {
        val (frames, seq) = sim.loraTx(txSeq)
        txSeq = seq
        return frames.map { MeshAir.decode(it.bytes, channels) }
    }

    /**
     * The device's counters, as the diagnostics view asks for them, once
     * they satisfy [cond] (asked again every half second: an answer equal to
     * the last one leaves the Compose state untouched, so there is no "it
     * answered" to wait for).
     */
    private fun stats(ble: BleManager, cond: (com.raemond.opentrailpaper.ble.MeshStats) -> Boolean): com.raemond.opentrailpaper.ble.MeshStats {
        var asked = 0L
        waitFor("mesh stats to satisfy the check (last: ${ble.meshStats})", 10_000) {
            if (System.currentTimeMillis() - asked > 500) { ble.requestMeshStats(); asked = System.currentTimeMillis() }
            cond(ble.meshStats)
        }
        return ble.meshStats
    }

    /** What MeshChannelsSheet.scanned does with a scanned or pasted invite. */
    private fun joinFromInvite(ble: BleManager, url: String): Int {
        val first = MeshChannelUrl.parse(url).firstOrNull()
        assertNotNull("the invite parses: $url", first)
        assertTrue("the invite carries a key", first!!.psk.isNotEmpty())
        val slot = ble.firstFreeMeshChannel
        assertNotNull("a free channel slot", slot)
        ble.setMeshPrivateChannel(slot!!, first.name.ifEmpty { "Shared" }, first.psk)
        waitFor("the joined channel on the device", 10_000) { ble.meshChannels.any { it.index == slot } }
        return slot
    }

    private fun message(ble: BleManager, text: String): MeshMessage? = ble.meshMessages.firstOrNull { it.text == text }

    private fun node(ble: BleManager, num: Int): MeshNode? = ble.meshNodes.firstOrNull { it.num == num }

    private fun waitFor(what: String, timeoutMs: Long, cond: () -> Boolean) {
        val end = System.currentTimeMillis() + timeoutMs
        var last = System.nanoTime()
        while (true) {
            val now = System.nanoTime()
            shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(maxOf(1L, (now - last) / 1_000_000)))
            last = now
            if (cond()) return
            if (System.currentTimeMillis() > end) fail("timed out waiting for $what\n${dump()}")
            Thread.sleep(20)
        }
    }

    private fun runFor(ms: Long) {
        val end = System.currentTimeMillis() + ms
        waitFor("($ms ms)", ms + 5_000) { System.currentTimeMillis() >= end }
    }

    private fun dump(): String =
        "--- app ---\n" + (ble?.let { b ->
            "state=${b.state} link=${b.linkReady} ${b.meshState}\nchannels=${b.meshChannels.map { "${it.index}:${it.name}/0x%02x".format(it.hash) }}" +
                "\nnodes=${b.meshNodes.map { it.nodeId }}\nmessages=${b.meshMessages.map { "${it.id}/${it.channel}/${it.status}:'${it.text}'" }}\n"
        } ?: "") +
            "--- SimRadio ---\n${synchronized(log) { log.toString().lines().takeLast(40).joinToString("\n") }}" +
            "\n--- otp-sim console (tail) ---\n${sim.consoleTail(40)}"

    // ---- the radio switch --------------------------------------------------------------------

    /**
     * The Mesh screen's radio switch (and its "Turn on" button): flipped on,
     * the app shows the radio on — the device says so by itself, without the
     * rider leaving and reopening the screen — and flipped off, off.
     */
    @Ignore(
        "BUG: src/mesh_service.cpp:1151 applyEnabled() never sets `changed`, so no MESH_CHANGED push follows the " +
            "staged enable; the only state the app gets is the follow-up src/link.cpp onMesh (radio_on_tag, " +
            "follow = MT_STATE) sends at once, before or halfway through the mesh task applying it: it says off, or " +
            "enabled with radioUp still false. The rider taps \"Turn on the radio\" and the Mesh screen stays on " +
            "\"Mesh radio is off\" or shows \"No radio found\" (MeshScreen.kt:165) though the radio is up, until a " +
            "packet arrives or a reconnect makes the app ask again; turning it off likewise still shows it on.",
    )
    @Test
    fun radioSwitchShowsTheRadioOnWithoutReopeningTheScreen() {
        val ble = connect()
        assertFalse("mesh is off on a fresh device", ble.meshState.enabled)
        ble.setMeshEnabled(true)
        waitFor("the app to show the radio on", 10_000) { ble.meshState.enabled && ble.meshState.radioOk }
        ble.setMeshEnabled(false)
        waitFor("the app to show the radio off", 10_000) { !ble.meshState.enabled }
    }

    // ---- channels ----------------------------------------------------------------------------

    /**
     * A ride group's invite, as the stock Meshtastic app shares it
     * (`https://meshtastic.org/e/?add=true#<ChannelSet>`, with its LoRa config
     * alongside the channel): joined the way the Channels sheet joins it, the
     * device holds that channel with that name, key and hash; the group's
     * messages land in that channel's thread, a stranger's frame under the same
     * name but another key never shows, and the rider's reply goes out
     * encrypted with the group's key.
     */
    @Test
    fun inviteUrlJoinsThePrivateChannelAndItsMessagesStayOnIt() {
        val ble = connect()
        meshOn(ble)
        // meshtastic.ChannelSet { settings = 1 { psk = 2, name = 3 }, lora_config = 2 { use_preset = 1, region = 7 (US) } }
        val name = "TrailCrew"
        val settings = MeshAir.Pb().bytes(2, GROUP_KEY).string(3, name).build()
        val lora = MeshAir.Pb().varint(1, 1).varint(7, 1).build()
        val set = MeshAir.Pb().bytes(1, settings).bytes(2, lora).build()
        val url = "https://meshtastic.org/e/?add=true#" + Base64.getUrlEncoder().withoutPadding().encodeToString(set)

        val slot = joinFromInvite(ble, url)
        assertEquals("the first private slot", 1, slot)
        val group = Channel(name, GROUP_KEY)
        val ch = ble.meshChannels.first { it.index == slot }
        assertEquals(name, ch.name)
        assertArrayEquals("the device holds the invite's key", GROUP_KEY, ch.psk)
        assertEquals("the device's channel hash is Meshtastic's", group.hash, ch.hash)
        assertTrue(ch.isPrivate)
        assertEquals("256-bit key", ch.keyDescription)
        assertFalse("location sharing starts off", ch.sharesLocation)
        assertEquals("the primary is untouched", "LongFast", ble.meshChannels.first { it.isPrimary }.name)

        // A stranger who picked the same channel name but has another key.
        val stranger = Channel(name, MeshAir.hex("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"))
        assertNotEquals("the stranger's key gives another hash", group.hash, stranger.hash)
        assertNotEquals(longFast.hash, stranger.hash)
        val alex = VirtualNode(0x1234abcd, "Alex Rider", "ALX", group)
        val eve = VirtualNode(0x0bad0bad, "Eve", "EVE", stranger)
        val bob = VirtualNode(0x2222beef, "Bob", "BOB")
        hear(alex.text("Regroup at the summit"), rssi = -84, snr = 6)
        hear(eve.text("Free bikes at the car park"))
        hear(bob.text("Anyone on the ridge?"))
        hear(alex.text("Leaving in 5"))
        waitFor("the group's messages and the public one", 15_000) {
            message(ble, "Leaving in 5") != null && message(ble, "Anyone on the ridge?") != null
        }
        val m = message(ble, "Regroup at the summit")!!
        assertEquals("the group message is in the joined channel's thread", slot, m.channel)
        assertEquals(alex.num, m.from)
        assertTrue(m.isBroadcast)
        assertFalse(m.outgoing)
        assertEquals(-84, m.rssi)
        assertEquals(6, m.snr)
        assertEquals("heard directly", 0, m.hops)
        assertEquals(slot, message(ble, "Leaving in 5")!!.channel)
        assertEquals("the public message stays on the primary", 0, message(ble, "Anyone on the ridge?")!!.channel)
        assertNull("the stranger's frame never shows", message(ble, "Free bikes at the car park"))
        assertTrue("no message from the stranger at all", ble.meshMessages.none { it.from == eve.num })
        stats(ble) { it.rxOtherChannel >= 1 }      // the device counted it as another channel's

        // The rider answers in the group's thread.
        ble.sendMeshText("On my way", channel = slot)
        val out = deviceSends("the reply", 15_000, listOf(longFast, group)) { it.port == MeshAir.PORT_TEXT }
        assertEquals("encrypted for the group", group.hash, out.channelHash)
        assertTrue("decrypts with the group's key", out.channel === group)
        assertEquals("On my way", out.text)
        assertEquals(MeshAir.BROADCAST, out.dest)
        waitFor("the reply in the thread, sent", 10_000) { message(ble, "On my way")?.status == MeshMessage.Status.SENT }
        val reply = message(ble, "On my way")!!
        assertEquals(slot, reply.channel)
        assertTrue(reply.outgoing)
        assertEquals("the list row is that packet", out.id, reply.id)
    }

    /**
     * A joined channel whose one-byte hash happens to equal the primary's
     * (1 in 256 for any key; LongFast's is 0x08). Stock Meshtastic tries every
     * channel with a matching hash (Router::perhapsDecode); the device must
     * too, or the group can never be heard.
     */
    @Ignore(
        "BUG: src/mesh_service.cpp:346 chanForHash() returns only the FIRST channel whose hash matches, and " +
            "handleFrame() (:658, :701-707) tries only that one key. A joined private channel whose 8-bit hash equals " +
            "the primary's (1 in 256 keys; or two private channels colliding) is decrypted with LongFast's key, fails " +
            "to decode and is dropped as rxDropped: the rider joined the group from its invite but never sees a " +
            "single one of its messages, with no error anywhere.",
    )
    @Test
    fun joinedChannelWhoseHashMatchesThePrimaryStillGetsItsMessages() {
        val ble = connect()
        meshOn(ble)
        val name = "TrailCrew"
        // Pick the last key byte so xor(name) ^ xor(key) == LongFast's hash.
        val key = GROUP_KEY.copyOf()
        key[31] = 0
        key[31] = (MeshAir.channelHash(name, key) xor longFast.hash).toByte()
        val group = Channel(name, key)
        assertEquals("the collision is set up", longFast.hash, group.hash)
        val slot = joinFromInvite(ble, MeshChannelUrl.url(name, key))
        assertEquals(group.hash, ble.meshChannels.first { it.index == slot }.hash)

        val alex = VirtualNode(0x1234abcd, "Alex Rider", "ALX", group)
        val bob = VirtualNode(0x2222beef, "Bob", "BOB")
        hear(alex.text("Regroup at the summit"))
        hear(bob.text("Anyone on the ridge?"))
        waitFor("the public message", 15_000) { message(ble, "Anyone on the ridge?") != null }
        runFor(1_000)
        val m = message(ble, "Regroup at the summit")
        assertNotNull("the group's message arrived", m)
        assertEquals(slot, m!!.channel)
    }

    /**
     * Forgetting a private channel drops its thread. What the rider must not
     * see afterwards is that thread's messages turned into blank rows in the
     * public thread.
     */
    @Ignore(
        "BUG: src/mesh_service.cpp:1256-1258 applyPrivateChannel(remove) blanks the forgotten channel's messages " +
            "in place (msgs[i] = Message()) without removing them from the ring, so msgCount still counts them and " +
            "messageAt()/sendMeshHistory (src/link.cpp:601) stream them as id 0, from !00000000, text \"\", " +
            "channel 0 — the PRIMARY. After forgetting a ride group the rider's public thread gains one empty " +
            "message bubble per message the group had.",
    )
    @Test
    fun forgettingAPrivateChannelLeavesNoBlankMessagesInThePublicThread() {
        val ble = connect()
        meshOn(ble)
        val name = "TrailCrew"
        val group = Channel(name, GROUP_KEY)
        val slot = joinFromInvite(ble, MeshChannelUrl.url(name, GROUP_KEY))
        val alex = VirtualNode(0x1234abcd, "Alex Rider", "ALX", group)
        val bob = VirtualNode(0x2222beef, "Bob", "BOB")
        hear(bob.text("Anyone on the ridge?"))
        hear(alex.text("Regroup at the summit"))
        hear(alex.text("Leaving in 5"))
        waitFor("the messages", 15_000) { message(ble, "Leaving in 5") != null && message(ble, "Anyone on the ridge?") != null }

        ble.forgetMeshChannel(slot)
        waitFor("the channel gone", 10_000) { ble.meshChannels.none { it.index == slot } }
        ble.refreshMesh()
        runFor(1_500)
        assertTrue("the group's thread is gone", ble.meshMessages.none { it.channel == slot })
        assertNull(message(ble, "Regroup at the summit"))
        assertNotNull("the public message stays", message(ble, "Anyone on the ridge?"))
        val publicThread = ble.meshMessages.filter { it.channel == 0 }
        assertEquals(
            "the public thread holds only the public message: ${publicThread.map { "id=${it.id} from=${it.from} '${it.text}'" }}",
            listOf("Anyone on the ridge?"), publicThread.map { it.text },
        )
    }

    // ---- neighbours --------------------------------------------------------------------------

    /**
     * The Nodes sheet and the mesh map: each neighbour's names, where it says
     * it is (and how precisely), signal and hops from its latest packet, when
     * it was last heard — newest first — and the fallbacks for a node that has
     * not sent its NodeInfo or whose firmware does not say how far it is.
     * Ten neighbours, so the list crosses the device's 8-per-page boundary.
     */
    @Test
    fun neighboursShowNamesPositionsSignalAndHops() {
        val ble = connect()
        meshOn(ble)
        val t0 = System.currentTimeMillis()
        val alex = VirtualNode(0x1234abcd, "Alex Rider", "ALX")
        val bea = VirtualNode(0x5eedbea0.toInt(), "Bea on the Gravel", "BEA")
        val cy = VirtualNode(0x0c0ffee5, "", "")
        // Seven more, NodeInfo only.
        val crowd = (1..7).map { VirtualNode(0x40000000 + it, "Rider $it", "R$it") }

        hear(alex.nodeinfo(), rssi = -70, snr = 10)
        hear(alex.position(37.7749123, -122.4194456, altM = 52, sats = 9), rssi = -78, snr = 7)
        // Two relays away: sent with hop_limit 3, arrives with 1 left.
        hear(bea.nodeinfo(hopLimit = 1, hopStart = 3), rssi = -112, snr = -6)
        // A blurred position: only 13 bits of each coordinate published.
        hear(bea.position(51.5007, -0.1246, altM = -3, sats = 5, precisionBits = 13, hopLimit = 1, hopStart = 3), rssi = -112, snr = -6)
        for (n in crowd) hear(n.nodeinfo(), rssi = -95, snr = 2)
        // Firmware before Meshtastic 2.3 stamps no hop_start: its distance is unknown, not "direct".
        hear(cy.position(-33.8688, 151.2093, sats = 4, hopLimit = 3, hopStart = 0), rssi = -101, snr = -1)

        waitFor("all ten neighbours", 20_000) { ble.meshNodes.size == 10 && node(ble, cy.num)?.position != null }
        val t1 = System.currentTimeMillis()
        assertEquals("the device counts them too", 10, ble.meshState.nodeCount)
        assertEquals("each once", 10, ble.meshNodes.map { it.num }.toSet().size)
        assertEquals("newest first", cy.num, ble.meshNodes.first().num)
        assertEquals("oldest last", alex.num, ble.meshNodes.last().num)
        assertTrue("newest first throughout", ble.meshNodes.zipWithNext().all { (a, b) -> a.lastHeardMs >= b.lastHeardMs })
        for (n in ble.meshNodes) {
            assertTrue("${n.nodeId} heard during the test (${n.lastHeardMs - t0} ms)", n.lastHeardMs in (t0 - 1_500)..(t1 + 1_500))
        }

        val a = node(ble, alex.num)!!
        assertEquals("Alex Rider", a.longName)
        assertEquals("ALX", a.shortName)
        assertEquals("!1234abcd", a.nodeId)
        assertEquals("signal from its latest packet", -78, a.rssi)
        assertEquals(7, a.snr)
        assertEquals(0, a.hops)
        assertEquals("direct", a.hopsText)
        assertEquals("ALX · direct", a.mapLabel)
        val ap = a.position!!
        assertEquals(37.7749123, ap.latitude, 2e-7)
        assertEquals(-122.4194456, ap.longitude, 2e-7)
        assertEquals(52, ap.altitudeM)
        assertEquals(9, ap.satsInView)
        assertFalse("full precision", ap.isImprecise)
        assertNull(ap.uncertaintyM)
        assertTrue(ap.receivedMs in (t0 - 1_500)..(t1 + 1_500))

        val b = node(ble, bea.num)!!
        assertEquals("Bea on the Gravel", b.displayName)
        assertEquals(2, b.hops)
        assertEquals("BEA · 2 hops", b.mapLabel)
        assertEquals(-112, b.rssi)
        assertEquals(-6, b.snr)
        val bp = b.position!!
        assertEquals(-3, bp.altitudeM)
        assertEquals(13, bp.precisionBits)
        assertTrue("a blurred position is drawn as one", bp.isImprecise)
        assertTrue("uncertain by kilometres: ${bp.uncertaintyM}", bp.uncertaintyM!! > 1_000)

        val c = node(ble, cy.num)!!
        assertEquals("no NodeInfo yet: named by its id", "!0c0ffee5", c.displayName)
        assertEquals("fee5", c.shortLabel)
        assertNull("distance unknown, not \"direct\"", c.hopsText)
        assertEquals("fee5", c.mapLabel)
        assertEquals(-33.8688, c.position!!.latitude, 2e-7)
        assertEquals(151.2093, c.position!!.longitude, 2e-7)

        for ((i, n) in crowd.withIndex()) {
            val got = node(ble, n.num)!!
            assertEquals("Rider ${i + 1}", got.longName)
            assertNull("NodeInfo alone has no position", got.position)
        }
    }

    // ---- sending -----------------------------------------------------------------------------

    /**
     * A message to everyone: on the air it is a LongFast broadcast from the
     * device's node, no ack wanted, the text cut by the app to the device's
     * 200 bytes on a character boundary (never half an emoji); in the thread
     * it is the rider's own message, sent.
     */
    @Test
    fun broadcastFromTheAppGoesOnTheAirAndShowsSent() {
        val ble = connect()
        meshOn(ble)
        assertEquals("the node number comes from the board's MAC", NODE_NUM, ble.meshState.nodeNum)
        val text = "Café stop at km 42 — " + "🚲".repeat(60)    // 🚲 is 4 bytes in UTF-8
        // What fits in 200 bytes, whole code points only.
        val fits = StringBuilder()
        var i = 0
        while (i < text.length) {
            val cp = text.codePointAt(i)
            val next = fits.toString() + String(Character.toChars(cp))
            if (next.toByteArray().size > 200) break
            fits.setLength(0); fits.append(next)
            i += Character.charCount(cp)
        }
        val expected = fits.toString()
        assertTrue(expected.toByteArray().size in 197..200)

        ble.sendMeshText(text)
        val f = deviceSends("the broadcast", 15_000, listOf(longFast)) { it.port == MeshAir.PORT_TEXT }
        assertEquals(MeshAir.BROADCAST, f.dest)
        assertEquals(NODE_NUM, f.sender)
        assertEquals("on the public channel", longFast.hash, f.channelHash)
        assertFalse("a broadcast wants no ack", f.wantAck)
        assertEquals("a fresh packet: hop_start = hop_limit", f.hopStart, f.hopLimit)
        assertEquals("cut on a character boundary", expected, f.text)

        waitFor("the message sent", 10_000) { ble.meshMessages.any { it.outgoing && it.status == MeshMessage.Status.SENT } }
        val m = ble.meshMessages.single { it.outgoing }
        assertEquals(f.id, m.id)
        assertEquals(expected, m.text)
        assertEquals(0, m.channel)
        assertTrue(m.isBroadcast)
        assertEquals(NODE_NUM, m.from)
        assertFalse(ble.meshSendRejected)
        stats(ble) { it.tx >= 2 }      // the announcement and this
    }

    /**
     * A direct message to a node whose public key the device does not hold
     * goes out channel-encrypted (the legacy DM) with want_ack; the row is sent, then
     * delivered when the node's ACK arrives, and failed on its NAK.
     */
    @Test
    fun directMessageIsDeliveredOnTheNodesAckAndFailsOnItsNak() {
        val ble = connect()
        meshOn(ble)
        val dana = VirtualNode(0x0da7a001, "Dana", "DNA")
        hear(dana.nodeinfo())
        waitFor("Dana in the node list", 10_000) { node(ble, dana.num)?.shortName == "DNA" }

        ble.sendMeshText("Are you at the trailhead?", to = dana.num)
        val dm = deviceSends("the DM", 15_000, listOf(longFast)) { it.port == MeshAir.PORT_TEXT }
        assertEquals(dana.num, dm.dest)
        assertTrue("a DM wants an ack", dm.wantAck)
        assertEquals("legacy DM: encrypted with the channel key", longFast.hash, dm.channelHash)
        assertEquals("Are you at the trailhead?", dm.text)
        waitForRadioListening()

        waitFor("the DM in the thread", 10_000) { message(ble, "Are you at the trailhead?") != null }
        val sent = message(ble, "Are you at the trailhead?")!!
        assertEquals(dm.id, sent.id)
        assertEquals(dana.num, sent.to)
        assertFalse(sent.isBroadcast)
        assertEquals("transmitted, not yet delivered", MeshMessage.Status.SENT, sent.status)

        hear(dana.ack(NODE_NUM, dm.id))
        waitFor("the DM delivered", 10_000) { message(ble, "Are you at the trailhead?")?.status == MeshMessage.Status.ACKED }
        stats(ble) { it.acksRx == 1 }

        ble.sendMeshText("Hello?", to = dana.num)
        val dm2 = deviceSends("the second DM", 15_000, listOf(longFast)) { it.port == MeshAir.PORT_TEXT }
        assertEquals("Hello?", dm2.text)
        waitForRadioListening()
        hear(dana.ack(NODE_NUM, dm2.id, MeshAir.ROUTING_NO_RESPONSE))
        waitFor("the second DM failed", 10_000) { message(ble, "Hello?")?.status == MeshMessage.Status.FAILED }
        assertEquals("the first stays delivered", MeshMessage.Status.ACKED, message(ble, "Are you at the trailhead?")!!.status)
        assertEquals("a NAK is not an ack", 1, stats(ble) { true }.acksRx)
    }

    /** With the radio off the device refuses a send: the app says so, and nothing goes on the air. */
    @Test
    fun sendWithTheRadioOffIsRefusedAndNothingIsTransmitted() {
        val ble = connect()
        meshOn(ble)
        ble.setMeshEnabled(false)
        waitForMeshState(ble, "the radio off") { !it.enabled }
        sentSince(listOf(longFast))
        ble.sendMeshText("Anyone there?")
        waitFor("the refusal", 10_000) { ble.meshSendRejected }
        runFor(2_000)
        assertEquals("nothing transmitted", emptyList<String>(), sentSince(listOf(longFast)).map { it.toString() })
        assertNull("no row for a message that never went", message(ble, "Anyone there?"))

        // Back on, the same send goes.
        ble.setMeshEnabled(true)
        waitForMeshState(ble, "the radio up") { it.enabled && it.radioOk }
        ble.sendMeshText("Anyone there?")
        val f = deviceSends("the message", 15_000, listOf(longFast)) { it.port == MeshAir.PORT_TEXT }
        assertEquals("Anyone there?", f.text)
        waitFor("no longer refused", 5_000) { !ble.meshSendRejected && message(ble, "Anyone there?") != null }
    }

    // ---- the conversation over time ----------------------------------------------------------

    /**
     * A busy group chat — more messages than one history page (4) — part of
     * it while the phone was away (Bluetooth off): after the reconnect the
     * thread holds every message once, in the order heard, with the unread
     * count; opening the screen marks them read.
     */
    @Test
    fun busyChatIncludingWhileThePhoneWasAwayArrivesWholeAndInOrder() {
        val ble = connect()
        meshOn(ble)
        val alex = VirtualNode(0x1234abcd, "Alex Rider", "ALX")
        val bob = VirtualNode(0x2222beef, "Bob", "BOB")
        val texts = (1..10).map { "Climb ${it}: ${if (it % 2 == 0) "steady" else "attack"}" }
        for (t in texts.take(5)) hear((if (t.length % 2 == 0) alex else bob).text(t))
        waitFor("the first five", 15_000) { texts.take(5).all { message(ble, it) != null } }

        radio!!.setEnabled(false)
        waitFor("Bluetooth off", 10_000) { ble.state == ConnState.POWERED_OFF && !sim.status().get("phone_connected").asBoolean }
        for (t in texts.drop(5)) hear((if (t.length % 2 == 0) alex else bob).text(t))
        runFor(1_000)
        assertTrue("the app heard nothing while away", texts.drop(5).none { message(ble, it) != null })
        radio!!.setEnabled(true)
        waitFor("reconnected", 30_000) { connected(ble) }
        waitFor("all ten in the thread", 15_000) { texts.all { message(ble, it) != null } }

        val got = ble.meshMessages.filter { !it.outgoing }
        assertEquals("each once, in the order heard", texts, got.map { it.text })
        assertEquals("each its own packet", 10, got.map { it.id }.toSet().size)
        assertTrue("all on the public channel", got.all { it.channel == 0 })
        assertTrue("oldest first", got.zipWithNext().all { (a, b) -> a.timeMs <= b.timeMs })
        waitFor("ten unread", 5_000) { ble.meshState.unread == 10 }
        ble.markMeshRead()
        waitFor("read", 5_000) { ble.meshState.unread == 0 }
    }

    companion object {
        /** The board's MAC; the mesh node number is bytes 2-5, little-endian. */
        const val MAC = "02:00:12:34:56:78"
        const val NODE_NUM = 0x78563412

        /** A ride group's 256-bit key. */
        val GROUP_KEY: ByteArray = MeshAir.hex("6a1f3c9e2b7d4058a1c3e5f7092b4d6f8e0a2c4e6f8091a3b5c7d9e1f3a5b7c9")
    }
}
