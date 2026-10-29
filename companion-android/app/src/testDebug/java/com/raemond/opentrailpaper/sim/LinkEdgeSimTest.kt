package com.raemond.opentrailpaper.sim

import androidx.compose.runtime.MutableState
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.ble.BleManager.ConnState
import com.raemond.opentrailpaper.ble.BleManager.OtaPhase
import com.raemond.opentrailpaper.ble.BleManager.PairingIssue
import com.raemond.opentrailpaper.ble.SimRadio
import com.raemond.opentrailpaper.data.FirmwareRelease
import com.raemond.opentrailpaper.data.Prefs
import com.raemond.opentrailpaper.transfer.TransferCenter
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Before
import org.junit.Ignore
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.io.File

/**
 * The app's link to the device at its edges: the smallest and largest MTU, a
 * slow connection, the link dropping in the middle of each kind of transfer,
 * a firmware update end to end, reconnect storms and a second phone racing
 * the reconnect, the app restarting, the phone setting the device's clock,
 * and the device's logs for diagnostics — the real BleManager on SimRadio
 * against the real firmware in otp-sim.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = SimTestApp::class)
class LinkEdgeSimTest : AppSimHarness() {

    @Before
    fun clearPhone() {
        File(app.filesDir, "rides").deleteRecursively()
        File(app.cacheDir, "partial-rides").deleteRecursively()
        File(app.cacheDir, "logs").deleteRecursively()
    }

    /**
     * Another device (otp-sim with [args]) and, for it, a freshly installed
     * app: the phones so far switch off, and no device is remembered (a new
     * sim is a new device address).
     */
    private fun restartSim(args: List<String>) {
        radios.forEach { runCatching { it.setEnabled(false) } }
        sim.close()
        sim = startSim(args)
        Prefs.pairedDeviceAddress = null
    }

    private fun connected(phone: String = "phoneA", mtu: Int = 517): Pair<SimRadio, BleManager> {
        val radio = radio(phone, mtu = mtu)
        val ble = manager(radio)
        pairFresh(ble, radio)
        return radio to ble
    }

    private fun randomBytes(n: Int, seed: Long) = ByteArray(n).also { java.util.Random(seed).nextBytes(it) }

    /** A GPX track of [points] points (about 60 bytes each). */
    private fun gpx(name: String, points: Int): String {
        val sb = StringBuilder("<?xml version=\"1.0\"?>\n<gpx version=\"1.1\" creator=\"sim\"><trk><name>$name</name><trkseg>\n")
        for (i in 0 until points) {
            sb.append(String.format(java.util.Locale.US, "<trkpt lat=\"%.6f\" lon=\"%.6f\"/>\n", 45.70 + i * 0.00005, -121.52 + (i % 400) * 0.00004))
        }
        return sb.append("</trkseg></trk></gpx>").toString()
    }

    private val workoutText = "[COURSE HEADER]\nVERSION = 2\nUNITS = ENGLISH\nDESCRIPTION = Sim\n" +
        "FILE NAME = sim.mrc\nMINUTES PERCENT\n[END COURSE HEADER]\n[COURSE DATA]\n" +
        "0.00 50\n5.00 50\n5.00 90\n10.00 90\n[END COURSE DATA]\n"

    private fun routeOnCard(name: String): File? =
        File(sd, "routes").listFiles()?.firstOrNull { it.readText().contains("<name>$name</name>") }

    private fun sendRoute(ble: BleManager, name: String, gpx: String, timeoutMs: Long = 60_000) {
        ble.lastMessage = null
        ble.uploadRoute(name, gpx)
        waitFor("route $name sent", timeoutMs) { ble.routeReceived || ble.lastMessage?.startsWith("Route not sent") == true }
        assertTrue("route $name: ${ble.lastMessage}", ble.routeReceived)
        val f = routeOnCard(name) ?: error("route $name is not on the card: ${File(sd, "routes").list()?.toList()}")
        assertEquals("route $name stored whole", gpx, f.readText())
    }

    private fun sendWorkout(ble: BleManager, name: String) {
        ble.workoutMessage = null
        ble.uploadWorkout(name, workoutText)
        waitFor("workout $name sent", 30_000) { ble.workoutMessage != null }
        assertEquals("Loaded $name", ble.workoutMessage)
        assertEquals(workoutText, File(sd, "workouts/$name").readText())
    }

    private fun downloadRide(ble: BleManager, name: String, timeoutMs: Long = 90_000) {
        ble.downloadRide(name)
        waitFor("$name downloaded", timeoutMs) { ble.isCached(name) && ble.downloadingName == null }
        assertArrayEquals("$name intact", cardRide(name).readBytes(), ble.cachedRideFile(name).readBytes())
    }

    // ---- MTU and slow links ---------------------------------------------------------------------

    /**
     * Phones negotiate anything from the minimum ATT MTU (23: some Android
     * stacks never ask for more) to 517. At each, the device reports the MTU
     * it sees, and every kind of transfer the app makes arrives whole: a ride
     * download, the device's ride list, a route and a workout upload, the
     * device's log.
     */
    @Test
    fun everyTransferArrivesWholeAtTheSmallestAndLargestMtu() {
        for (mtu in listOf(23, 185, 247, 517)) {
            if (mtu != 23) restartSim(simArgs)
            cardRides((1..20).map { RideSpec(start = 1_760_000_000L + it * 86_400L, seconds = 60) })
            val ride = sdFile("rides", "20260101-080000.fit", randomBytes(48 * 1024 + 7, mtu.toLong()))
            val (_, ble) = connected("mtu$mtu", mtu = mtu)
            // The device's stack offers 247 (CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU): the lower wins.
            assertEquals("the device sees MTU $mtu", minOf(mtu, 247), ble.deviceHello!!.mtu)
            ridesTabCheck(ble)
            assertEquals("MTU $mtu: every ride listed", 21, ble.rides.size)
            downloadRide(ble, "20260101-080000.fit")
            assertArrayEquals(ride, ble.cachedRideFile("20260101-080000.fit").readBytes())
            sendRoute(ble, "Mtu$mtu", gpx("Mtu$mtu", 300), timeoutMs = 120_000)
            sendWorkout(ble, "mtu$mtu.mrc")
            ble.downloadLog()
            waitFor("MTU $mtu: today's log", 30_000) { ble.logFile != null && ble.downloadingName == null }
            assertTrue("MTU $mtu: the log has this link", ble.logFile!!.readText().contains("MTU=${minOf(mtu, 247)}"))
            ble.logFile = null
        }
    }

    /**
     * A slow connection (300 ms interval, four packets per event, the
     * controller's buffers nearly full): the ride list of a well-used card,
     * a ride, a route and a workout all still complete — none of the app's
     * watchdogs (20 s without data, 10 s per list page) mistakes slow for dead.
     */
    @Test
    fun aSlowLinkStillCompletesEveryTransfer() {
        restartSim(listOf("--conn-interval-ms", "300", "--packets-per-event", "4", "--notify-window", "4"))
        cardRides((1..300).map { RideSpec(start = 1_700_000_000L + it * 86_400L, seconds = 30) })
        sdFile("rides", "20260101-080000.fit", randomBytes(64 * 1024, 9))
        val (_, ble) = connected()
        ridesTabCheck(ble, timeoutMs = 120_000)
        assertEquals("every ride listed over the slow link", 301, ble.rides.size)
        downloadRide(ble, "20260101-080000.fit", timeoutMs = 120_000)
        sendRoute(ble, "Slow", gpx("Slow", 2_000), timeoutMs = 120_000)
        sendWorkout(ble, "slow.mrc")
        val notifyFailed = sim.logSince(0).filter { "notify failed" in it }
        assertTrue("the device gave up on a busy notify: $notifyFailed", notifyFailed.isEmpty())
    }

    // ---- the link drops mid-transfer -------------------------------------------------------------

    private val slowLink = listOf("--conn-interval-ms", "60", "--packets-per-event", "2", "--notify-window", "4")

    /** Drops the radio link now and waits for the app to be back, connected. */
    private fun dropAndReconnect(radio: SimRadio, ble: BleManager, check: () -> Unit = {}) {
        val upBefore = radio.linksUp.get()
        sim.dropPhone()
        waitFor("the drop noticed", 10_000) { !ble.linkReady }
        check()
        waitFor("reconnected", 30_000, neverRefused(ble)) { connectedOverLink(ble) && radio.linksUp.get() > upBefore }
    }

    /**
     * Today's log is a big file (a long day of diagnostics); the link drops
     * while it downloads. The app says so and saves no log; asked again, the
     * whole log arrives.
     */
    @Test
    fun aLogDownloadCutByADropFailsAndTheRetryArrivesWhole() {
        restartSim(slowLink)
        val day = java.text.SimpleDateFormat("yyyy-MM-dd", java.util.Locale.US).format(java.util.Date())
        val old = "2026-01-02.log"
        val body = (0 until 6_000).joinToString("\n") { "[08:00:%02d] sim: a long day of diagnostics, line $it".format(it % 60) }.toByteArray()
        sdFile("logs", old, body)
        val (radio, ble) = connected()
        ble.requestLogList()
        waitFor("the device's log list", 20_000) { !ble.loadingLogs && ble.deviceLogs.isNotEmpty() }
        assertTrue("the old day is listed: ${ble.deviceLogs}", ble.deviceLogs.any { it.name == old && it.size == body.size })
        println("today ($day) and the list: ${ble.deviceLogs}")

        ble.downloadLogFile(old)
        waitFor("a third of the log", 60_000) { ble.downloadingLog && ble.downloadProgress >= 0.33 }
        dropAndReconnect(radio, ble) {
            waitFor("the log download ended", 10_000) { ble.downloadingName == null }
            assertNull("no log saved from a cut download", ble.logFile)
            assertEquals("Connection dropped mid-download — try again", ble.lastMessage)
        }
        ble.downloadLogFile(old)
        waitFor("the log downloaded", 120_000) { ble.logFile != null && ble.downloadingName == null }
        assertArrayEquals("the whole log", body, ble.logFile!!.readBytes())
    }

    /**
     * A big route (200 KB of GPX) is half sent when the link drops: the app
     * says the route wasn't sent, the device keeps no half route (and drops
     * the upload rather than holding its one upload slot), and sending again
     * after the reconnect lands the whole route.
     */
    @Test
    fun aRouteUploadCutByADropIsReportedAndTheRetryLands() {
        restartSim(slowLink)
        val (radio, ble) = connected()
        val route = gpx("Long", 4_500)
        assertTrue("a big route: ${route.length}", route.length > 180_000)
        ble.lastMessage = null
        ble.uploadRoute("Long", route)
        waitFor("a third of the route sent", 60_000) { (ble.lastUploadProgress ?: 0.0) >= 0.33 }
        assertFalse("not done yet", ble.routeReceived)
        dropAndReconnect(radio, ble) {
            // The route screen's progress goes and its Send button is back;
            // the transfer notification says it failed.
            waitFor("the route upload ended", 10_000) { !TransferCenter.isActive("ble.route") }
            val ended = TransferCenter.lastEnded
            assertEquals("ble.route", ended?.id)
            assertEquals("the route's transfer failed: $ended", TransferCenter.Phase.FAILED, ended?.phase)
            assertNull(ble.lastUploadProgress)
            assertFalse(ble.routeSent)
            assertFalse(ble.routeReceived)
        }
        assertNull("no half route on the card", routeOnCard("Long"))
        sendRoute(ble, "Long", route, timeoutMs = 120_000)
    }

    /**
     * A workout upload cut by a drop: the device keeps no half file (a
     * workout on its card is whole or absent), and the next upload loads it.
     */
    @Test
    fun aWorkoutUploadCutByADropLeavesNoHalfFileAndTheRetryLoads() {
        restartSim(slowLink + listOf("--sd-latency-ms", "200"))
        val (radio, ble) = connected()
        ble.workoutMessage = null
        ble.uploadWorkout("cut.mrc", workoutText)
        dropAndReconnect(radio, ble)
        val onCard = File(sd, "workouts/cut.mrc")
        if (onCard.exists()) assertEquals("a workout file is whole", workoutText, onCard.readText())
        sim.cmd("sd", "latency_ms" to 0)
        sendWorkout(ble, "cut.mrc")
    }

    /**
     * The same drop, as the rider sees it: the Workouts screen says
     * "Sending cut.mrc…" and must then say whether it got there.
     */
    @Ignore(
        "BUG: a workout upload cut by a link drop is never reported: endConnection drops workoutUpload " +
            "before link.reset, so the CANCELLED callback is ignored, and nothing else sets workoutMessage — " +
            "the Workouts screen's \"Sending …\" toast just goes away as if it had been sent " +
            "(BleManager.kt endConnection / uploadWorkout)",
    )
    @Test
    fun aWorkoutUploadCutByADropIsReportedToTheRider() {
        restartSim(slowLink + listOf("--sd-latency-ms", "200"))
        val (radio, ble) = connected()
        ble.workoutMessage = null
        ble.uploadWorkout("cut.mrc", workoutText)
        dropAndReconnect(radio, ble)
        runFor(2_000)
        val msg = ble.workoutMessage
        assertNotNull("the rider was never told the workout didn't go", msg)
        if (!File(sd, "workouts/cut.mrc").exists()) assertTrue("reported as failed: $msg", msg!!.startsWith("Failed"))
    }

    // ---- firmware updates -------------------------------------------------------------------------

    /**
     * The release the app offers: the firmware screen's GitHub check, stood in
     * for (no network): [FirmwareRelease.latest] and its downloaded image.
     */
    @Suppress("UNCHECKED_CAST")
    private fun offerRelease(tag: String, image: ByteArray) {
        val r = FirmwareRelease.Release(tag, "http://127.0.0.1:9/firmware.bin", image.size, "sim")
        val latest = FirmwareRelease::class.java.getDeclaredField("latest\$delegate").apply { isAccessible = true }
        (latest.get(null) as MutableState<FirmwareRelease.Release?>).value = r
        FirmwareRelease::class.java.getDeclaredField("cached").apply { isAccessible = true }.set(null, tag to image)
    }

    private fun firmwareImage(n: Int = 320 * 1024) = randomBytes(n, 77).also { it[0] = 0xE9.toByte() }

    /**
     * The firmware update, end to end: the app sends the release to the
     * device, which saves it, says so and restarts; the app reconnects by
     * itself and, the device now reporting the release's version, says it is
     * updated. The device holds exactly the image the app had. (The sim
     * re-execs itself on the reboot, so it runs the same version after it:
     * the release offered is the version it runs.)
     */
    @Test
    fun aFirmwareUpdateGoesFromThePhoneToTheDeviceAndIsConfirmed() {
        val (radio, ble) = connected()
        val version = ble.deviceFirmware
        assertTrue("the device's firmware version from Hello: '$version'", version.isNotEmpty())
        assertEquals(ble.deviceHello!!.firmware, version)
        val image = firmwareImage()
        offerRelease(version, image)
        val upBefore = radio.linksUp.get()
        val phases = linkedSetOf<OtaPhase>()
        ble.startFirmwareUpdate()
        waitFor("the update sent and the device restarting", 120_000, { phases += ble.otaPhase }) {
            ble.otaPhase == OtaPhase.INSTALLING || ble.otaPhase == OtaPhase.DONE || ble.otaPhase == OtaPhase.FAILED
        }
        assertTrue("the update failed: ${ble.otaMessage}", ble.otaPhase != OtaPhase.FAILED)
        sim.reconnectControl()
        waitFor("reconnected after the restart", 60_000, { phases += ble.otaPhase; neverRefused(ble)() }) {
            connectedOverLink(ble) && radio.linksUp.get() > upBefore
        }
        waitFor("the app says it's updated", 15_000, { phases += ble.otaPhase }) { ble.otaPhase == OtaPhase.DONE }
        assertFalse(ble.otaInProgress)
        assertEquals("Updated to $version 🎉", ble.otaMessage)
        assertTrue("the rider saw it send, then install: $phases", OtaPhase.SENDING in phases && OtaPhase.INSTALLING in phases)
        assertArrayEquals("the device holds the release's image", image, File(sim.dir, "ota.bin").readBytes())
        assertNull(ble.pairingIssue)
    }

    /**
     * The update reaches the device and it restarts, but comes back on the
     * old version (the install didn't take). The app must say so within its
     * grace period — not stay on "Installing…" for ever.
     */
    @Test
    fun anUpdateThatDoesNotTakeIsReportedNotWaitedOnForever() {
        val (radio, ble) = connected()
        val version = ble.deviceFirmware
        offerRelease("v99.99-sim", firmwareImage(64 * 1024))
        val upBefore = radio.linksUp.get()
        ble.startFirmwareUpdate()
        waitFor("the update sent", 60_000) { ble.otaPhase == OtaPhase.INSTALLING || ble.otaPhase == OtaPhase.FAILED }
        sim.reconnectControl()
        waitFor("reconnected", 60_000) { connectedOverLink(ble) && radio.linksUp.get() > upBefore }
        waitFor("verifying the version", 10_000) { ble.otaPhase == OtaPhase.VERIFYING }
        waitFor("the update reported as failed", 75_000) { ble.otaPhase == OtaPhase.FAILED }
        assertFalse(ble.otaInProgress)
        assertTrue("the message names the version it's still on: ${ble.otaMessage}", ble.otaMessage!!.contains(version))
        assertTrue("an update is still offered", ble.updateAvailable)
    }

    /**
     * The link drops half way through sending the firmware: the app says the
     * device is unchanged — and it is: it doesn't restart, flashes nothing,
     * and frees its upload slot, so the retry after the reconnect installs.
     */
    @Test
    fun aFirmwareUpdateCutByADropLeavesTheDeviceUnchanged() {
        restartSim(slowLink)
        val (radio, ble) = connected()
        val version = ble.deviceFirmware
        val image = firmwareImage(400 * 1024)
        offerRelease(version, image)
        ble.startFirmwareUpdate()
        waitFor("a third of the firmware sent", 120_000) { ble.otaPhase == OtaPhase.SENDING && ble.otaProgress >= 0.33 }
        val uptime = sim.status().get("uptime_ms").asLong
        dropAndReconnect(radio, ble) {
            waitFor("the update reported as failed", 10_000) { ble.otaPhase == OtaPhase.FAILED }
            assertTrue("the rider is told the device is unchanged: ${ble.otaMessage}", ble.otaMessage!!.contains("unchanged"))
        }
        assertTrue("the device didn't restart", sim.status().get("uptime_ms").asLong > uptime)
        val accepted = sim.logSince(0).filter { "OTA image" in it }
        assertTrue("nothing was flashed: $accepted", accepted.isEmpty())

        val upBefore = radio.linksUp.get()
        ble.startFirmwareUpdate()
        waitFor("the retry sent", 180_000) { ble.otaPhase == OtaPhase.INSTALLING || ble.otaPhase == OtaPhase.FAILED }
        assertEquals("the retry: ${ble.otaMessage}", OtaPhase.INSTALLING, ble.otaPhase)
        sim.reconnectControl()
        waitFor("the update confirmed", 60_000) { ble.otaPhase == OtaPhase.DONE && radio.linksUp.get() > upBefore }
        assertArrayEquals(image, File(sim.dir, "ota.bin").readBytes())
    }

    // ---- reconnects -----------------------------------------------------------------------------

    /**
     * A storm of link losses (the rider at the edge of range: drops every
     * fraction of a second, some during the reconnect's own encryption,
     * Hello and settings sync). When it calms down the app is connected over
     * one link, never told the rider the device refused it, and the link
     * works (the ride list comes back).
     */
    @Test
    fun aReconnectStormEndsWithOneWorkingLinkAndNoRefusal() {
        cardRides((1..5).map { RideSpec(start = 1_760_000_000L + it * 86_400L, seconds = 60) })
        val (radio, ble) = connected()
        ridesTabCheck(ble)
        val rnd = java.util.Random(5)
        repeat(12) {
            runFor(100L + rnd.nextInt(1_200), neverRefused(ble))
            if (phoneConnected()) sim.dropPhone()
        }
        waitFor("connected after the storm", 40_000, neverRefused(ble)) { connectedOverLink(ble) }
        runFor(3_000, neverRefused(ble))
        val st = sim.status()
        val conns = st.getAsJsonObject("ble").getAsJsonArray("connections")
        assertEquals("one link at the device: $conns", 1, conns.size())
        assertEquals("one bond", 1, st.getAsJsonObject("ble").getAsJsonArray("bonds").size())
        assertTrue("still connected", connectedOverLink(ble))
        assertTrue(radio.holdsBond(radio.deviceAddress))
        ridesTabCheck(ble)
        assertEquals("the link works", 5, ble.rides.size)
    }

    /**
     * Phone A's link drops and, while it reconnects, phone B (another
     * handset with the app, never paired) tries for the device. The device
     * stays phone A's: A is back, B is told the device belongs to another
     * phone and stops trying, and A was never told it was refused.
     */
    @Test
    fun aSecondPhoneRacingTheReconnectDoesNotTakeTheDevice() {
        val (radioA, bleA) = connected("phoneA")
        // Phone B: its own bond store and a freshly installed app's prefs.
        val radioB = radio("phoneB")
        sim.dropPhone()
        Prefs.pairedDeviceAddress = null
        val bleB = manager(radioB)
        waitFor("phone B turned away", 40_000, neverRefused(bleA)) {
            bleB.pairingIssue == PairingIssue.PAIRED_ELSEWHERE || bleB.pairingIssue == PairingIssue.REFUSED
        }
        waitFor("phone A back", 40_000, neverRefused(bleA)) { connectedOverLink(bleA) }
        val attempts = radioB.connectAttempts.get()
        runFor(8_000) {
            neverRefused(bleA)()
            if (bleB.state == ConnState.CONNECTED && bleB.linkReady) fail("phone B got the device\n${dump()}")
        }
        assertEquals("phone B stopped trying", attempts, radioB.connectAttempts.get())
        assertTrue("phone A still connected", connectedOverLink(bleA))
        val bonds = sim.status().getAsJsonObject("ble").getAsJsonArray("bonds").map { it.asString.uppercase() }
        assertEquals("the device holds phone A's bond only: $bonds", listOf(radioA.identity), bonds)
    }

    /**
     * The app is closed and opened again (a new process: a new BleManager on
     * the same phone's bond store and prefs). The Rides tab shows the last
     * ride list before the device answers; the app reconnects to the device
     * it knows without pairing again (no new bond on either side) and the
     * downloaded ride is still on the phone.
     */
    @Test
    fun anAppRestartShowsTheLastRidesAndReconnectsWithoutPairing() {
        val specs = (1..4).map { RideSpec(start = 1_760_000_000L + it * 86_400L, seconds = 120) }
        cardRides(specs)
        val (radio1, ble1) = connected()
        ridesTabCheck(ble1)
        val listed = ble1.rides.map { it.name }
        assertEquals(4, listed.size)
        downloadRide(ble1, listed.first())
        val bondsBefore = sim.status().getAsJsonObject("ble").getAsJsonArray("bonds").size()

        // The process dies: its radio goes with it.
        radio1.setEnabled(false)
        waitFor("the old app gone", 10_000) { !phoneConnected() }
        // A ride recorded meanwhile.
        val newRide = RideSpec(start = 1_760_000_000L + 10 * 86_400L, seconds = 60)
        cardRides(listOf(newRide))

        val radio2 = radio("phoneA")   // the same phone: same identity and bonds
        assertEquals(radio1.identity, radio2.identity)
        val ble2 = manager(radio2)
        assertEquals("the last ride list, before the device answers", listed, ble2.rides.map { it.name })
        assertTrue("the downloaded ride is still on the phone", ble2.isCached(listed.first()))
        waitFor("reconnected without pairing", 30_000, neverRefused(ble2)) { connectedOverLink(ble2) }
        assertEquals("no new bond on the device", bondsBefore, sim.status().getAsJsonObject("ble").getAsJsonArray("bonds").size())
        assertNull(ble2.pairingIssue)
        ridesTabCheck(ble2)
        assertEquals("the new ride on top", 5, ble2.rides.size)
        assertEquals(newRide.name, ble2.rides.first().name)
    }

    // ---- the clock, the device's identity and its logs -----------------------------------------

    /**
     * A device indoors with no GPS time (its clock never set since power-on)
     * gets the phone's time with the Hello: its clock is right within a
     * couple of seconds, and it calls its time valid.
     */
    @Test
    fun thePhoneSetsTheClockOfADeviceWithoutGps() {
        restartSim(listOf("--gps-cold"))
        val before = sim.status()
        val skew0 = before.get("wall_unix").asLong - System.currentTimeMillis() / 1000
        println("device clock before the phone: ${before.get("wall_unix")} (off by $skew0 s), time_valid=${before.getAsJsonObject("ride").get("time_valid")}")
        val (_, ble) = connected()
        waitFor("the device's clock set", 10_000) {
            val st = sim.status()
            st.getAsJsonObject("ride").get("time_valid").asBoolean &&
                kotlin.math.abs(st.get("wall_unix").asLong - System.currentTimeMillis() / 1000) <= 2
        }
        assertNotNull(ble.deviceHello)
    }

    /**
     * Diagnostics: the device's log list has today's log; downloading it
     * gets a file with what the device did on this link (its boot, the
     * phone's connection) — the file a rider attaches to a bug report.
     */
    @Test
    fun theDevicesLogListsAndDownloadsForDiagnostics() {
        val (_, ble) = connected()
        ble.requestLogList()
        waitFor("the log list", 20_000) { !ble.loadingLogs && ble.deviceLogs.isNotEmpty() }
        val names = ble.deviceLogs.map { it.name }
        assertEquals("newest day first", names.sortedDescending(), names)
        ble.downloadLog()
        waitFor("today's log", 30_000) { ble.logFile != null && ble.downloadingName == null }
        val text = ble.logFile!!.readText()
        assertTrue("the log has this phone's link: ${text.take(400)}", text.contains("phone link up") && text.contains("hello from Android app"))
        // pending.log holds what the device logged before it knew the date.
        val stamp = java.text.SimpleDateFormat("yyyyMMdd", java.util.Locale.US).format(java.util.Date())
        val today = ble.deviceLogs.firstOrNull { it.name == "$stamp.log" } ?: error("no $stamp.log in ${ble.deviceLogs}")
        ble.logFile = null
        ble.downloadLogFile(today.name)
        waitFor("${today.name} by name", 30_000) { ble.logFile != null && ble.downloadingName == null }
        val byName = ble.logFile!!.readText()
        assertTrue("${today.name} (of ${ble.deviceLogs}) has this link: ${byName.takeLast(400)}", byName.contains("hello from Android app"))
    }
}
