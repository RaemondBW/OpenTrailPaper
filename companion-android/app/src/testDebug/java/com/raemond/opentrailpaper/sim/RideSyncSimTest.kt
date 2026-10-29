package com.raemond.opentrailpaper.sim

import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.ble.BleManager.ConnState
import com.raemond.opentrailpaper.data.FitDecoder
import com.raemond.opentrailpaper.data.RideListingStore
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Ignore
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.io.File
import kotlin.math.abs
import kotlin.math.sin

/**
 * The Rides tab against the real firmware: the device's ride list, FIT
 * downloads (whole, resumed, re-fetched), deleting on the device from the app,
 * rides recorded while the app was away, and a card with very many rides —
 * the app's BleManager and FitDecoder over SimRadio, the device's link and
 * FitWriter in otp-sim.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = SimTestApp::class)
class RideSyncSimTest : AppSimHarness() {

    @Before
    fun clearPhoneRides() {
        File(app.filesDir, "rides").deleteRecursively()
        File(app.cacheDir, "partial-rides").deleteRecursively()
    }

    private fun partialFile(name: String) = File(app.cacheDir, "partial-rides/$name")

    private fun connected(phone: String = "phoneA"): BleManager {
        val radio = radio(phone)
        val ble = manager(radio)
        pairFresh(ble, radio)
        return ble
    }

    private fun downloadAndWait(ble: BleManager, name: String, timeoutMs: Long = 60_000) {
        ble.downloadRide(name)
        waitFor("$name downloaded", timeoutMs) { ble.isCached(name) && ble.downloadingName == null }
    }

    // ---- a ride recorded on the device --------------------------------------------------------

    /**
     * A ride the rider records and saves on the device shows up in the Rides
     * tab with the card's size, downloads byte for byte, and the app's
     * FitDecoder reads the numbers the device showed on its ride summary
     * (distance, moving time, elapsed time) and a track along the road ridden.
     */
    @Test
    fun aRideRecordedOnTheDeviceDownloadsAndReadsAsTheDeviceSummarisedIt() {
        val ble = connected()
        ridesTabCheck(ble)
        assertTrue("a factory-fresh card has no rides: ${ble.rides}", ble.rides.isEmpty())

        val summary = recordRideOnDevice(seconds = 120, kmh = 30.0, pump = { runFor(it) })
        val onCard = File(sd, "rides").listFiles()!!.filter { it.name.endsWith(".fit") }
        assertEquals("one ride on the card", 1, onCard.size)
        val ride = onCard.single()

        // A new link check (the tab is opened again after the ride).
        ble.refreshRides()
        waitFor("the ride listed", 20_000) { !ble.loadingRides && ble.rides.size == 1 }
        assertEquals(ride.name, ble.rides.single().name)
        assertEquals("listed with the card's size", ride.length().toInt(), ble.rides.single().size)

        downloadAndWait(ble, ride.name)
        val bytes = ble.cachedRideFile(ride.name).readBytes()
        assertArrayEquals("the downloaded FIT is the card's", ride.readBytes(), bytes)
        assertEquals("${ride.name} ready", ble.lastMessage)

        val p = checkNotNull(FitDecoder.decode(bytes)) { "the ride doesn't decode" }
        val devKm = summary.get("distance_m").asDouble / 1000
        val devMoving = summary.get("moving_s").asDouble
        val devElapsed = summary.get("elapsed_s").asDouble
        // The moment between reading the device and its SAVE: a second or two.
        assertTrue("distance: app ${p.distanceKm} km, device $devKm km", abs(p.distanceKm - devKm) < 0.025)
        assertTrue("moving time: app ${p.movingTime} s, device $devMoving s", abs(p.movingTime - devMoving) <= 3.0)
        // Elapsed comes from the records' GPS timestamps, which at the sim's 6x
        // clock run a few percent apart from the device's own ride timer.
        assertTrue("elapsed: app ${p.duration} s, device $devElapsed s", abs(p.duration - devElapsed) <= 3.0 + 0.06 * devElapsed)
        val avg = devKm / (devMoving / 3600)
        assertTrue("average speed: app ${p.avgSpeedKmh}, device $avg km/h", abs(p.avgSpeedKmh - avg) < 1.0)
        assertTrue("a 30 km/h ride has a top speed near 30: ${p.maxSpeedKmh}", p.maxSpeedKmh in 25.0..36.0)
        assertTrue("a point a second or so: ${p.points.size}", p.points.size >= devElapsed * 0.8)
        // The track: east from 45.70 N, 121.52 W along the line the GPS rode.
        for (pt in p.points) {
            assertTrue("off the road: ${pt.coordinate}", abs(pt.coordinate.lat - 45.70) < 0.001)
            assertTrue("off the road: ${pt.coordinate}", pt.coordinate.lon in -121.5205..-121.47)
        }
        assertTrue("the track moves east", p.points.last().coordinate.lon > p.points.first().coordinate.lon + 0.005)
        assertNull("no heart-rate strap, no heart rate", p.avgHeartRate)
        assertNull("no power meter, no power", p.avgPower)
    }

    /**
     * FIT files the firmware's FitWriter wrote (with a heart-rate strap and a
     * power meter, and without) decode to the session the device wrote:
     * start, moving time, distance, average heart rate and power, ascent.
     */
    @Test
    fun deviceFitFilesDecodeToTheSessionTheDeviceWrote() {
        val now = System.currentTimeMillis() / 1000 / 60 * 60
        val specs = listOf(
            RideSpec(start = now - 86_400 * 3, seconds = 3_600, kmh = 27.0, hr = 145, watts = 210),
            RideSpec(start = now - 86_400 * 2, seconds = 1_800, kmh = 22.0, hr = 0, watts = 0),
            RideSpec(start = now - 86_400, seconds = 2_400, kmh = 31.0, hr = 160, watts = 0),
        )
        val files = cardRides(specs)
        val ble = connected()
        ridesTabCheck(ble)
        assertEquals(specs.map { it.name }.sortedDescending(), ble.rides.map { it.name })
        for ((spec, file) in specs.zip(files)) {
            downloadAndWait(ble, spec.name)
            val bytes = ble.cachedRideFile(spec.name).readBytes()
            assertArrayEquals(file.readBytes(), bytes)
            val p = checkNotNull(FitDecoder.decode(bytes)) { "${spec.name} doesn't decode" }

            // What card-tool's FitWriter was fed (sim/host/cardtool_sim.cpp).
            var dist = 0.0
            var hrSum = 0.0
            var wSum = 0.0
            for (t in 0 until spec.seconds) {
                dist += spec.kmh / 3.6 * (0.85 + 0.15 * sin(t / 37.0))
                hrSum += (spec.hr + 8 * sin(t / 60.0)).toInt()
                wSum += (spec.watts + 30 * sin(t / 11.0)).toInt()
            }
            val what = "${spec.name} (${spec.hr} bpm, ${spec.watts} W)"
            assertEquals("$what start", spec.start * 1000, p.startEpochMs)
            assertEquals("$what moving time", spec.seconds.toDouble(), p.movingTime, 0.001)
            assertEquals("$what elapsed", (spec.seconds - 1).toDouble(), p.duration, 0.001)
            assertEquals("$what distance", dist / 1000, p.distanceKm, 0.001)
            assertEquals("$what a point a second", spec.seconds, p.points.size)
            assertEquals("$what ascent", 60.0, p.ascentM, 0.5)
            if (spec.hr > 0) assertEquals("$what heart rate", (hrSum / spec.seconds).toInt(), p.avgHeartRate)
            else assertNull("$what heart rate", p.avgHeartRate)
            if (spec.watts > 0) {
                assertEquals("$what power", (wSum / spec.seconds).toInt(), p.avgPower)
                assertEquals("$what NP", (wSum / spec.seconds).toInt(), p.normPower)
            } else {
                assertNull("$what power", p.avgPower)
            }
            assertTrue("$what speed ${p.avgSpeedKmh}", abs(p.avgSpeedKmh - dist / spec.seconds * 3.6) < 0.01)
        }
    }

    // ---- many rides ------------------------------------------------------------------------------

    /**
     * A card with years of rides (600: thirty-eight FileList pages): the
     * Rides tab gets every one, newest first, each with its size, and keeps
     * the list for the next launch.
     */
    @Test
    fun aCardWithHundredsOfRidesListsEveryOneNewestFirst() {
        val first = 1_700_000_000L
        val specs = (0 until 600).map { RideSpec(start = first + it * 86_400L + (it % 7) * 3_600, seconds = 30) }
        cardRides(specs)
        val ble = connected()
        ridesTabCheck(ble, timeoutMs = 60_000)
        val want = specs.map { it.name }.sortedDescending()
        assertEquals("every ride, newest first", want, ble.rides.map { it.name })
        for (r in ble.rides) assertEquals("${r.name} size", cardRide(r.name).length().toInt(), r.size)
        // Kept for the next launch, under this device.
        assertEquals(want, RideListingStore.load(ble.settingsDeviceId).map { it.name })

        // The oldest ride, at the bottom of the list, downloads like the newest.
        val oldest = want.last()
        downloadAndWait(ble, oldest)
        assertArrayEquals(cardRide(oldest).readBytes(), ble.cachedRideFile(oldest).readBytes())
    }

    // ---- deleting ----------------------------------------------------------------------------------

    /**
     * Deleting a ride in the app deletes it on the device's card and on the
     * phone; the next check of the device doesn't bring it back, and the
     * other rides stay.
     */
    @Test
    fun aRideDeletedInTheAppIsGoneFromTheCardAndThePhone() {
        val now = System.currentTimeMillis() / 1000 / 60 * 60
        val specs = (1..3).map { RideSpec(start = now - it * 86_400L, seconds = 300) }
        cardRides(specs)
        val ble = connected()
        ridesTabCheck(ble)
        assertEquals(3, ble.rides.size)
        val (keep, downloaded, notDownloaded) = specs.map { it.name }
        downloadAndWait(ble, downloaded)

        ble.deleteRide(downloaded)
        ble.deleteRide(notDownloaded)
        assertEquals("gone from the list at once", listOf(keep), ble.rides.map { it.name })
        assertFalse("the phone's copy is gone", ble.isCached(downloaded))
        waitFor("both deleted on the card", 10_000) { !cardRide(downloaded).exists() && !cardRide(notDownloaded).exists() }
        assertTrue("the other ride stays on the card", cardRide(keep).exists())

        // Reconnect: the device's list doesn't bring them back.
        val upBefore = radios.single().linksUp.get()
        sim.dropPhone()
        waitFor("reconnected", 30_000) { connectedOverLink(ble) && radios.single().linksUp.get() > upBefore }
        ridesTabCheck(ble)
        assertEquals(listOf(keep), ble.rides.map { it.name })
    }

    /**
     * Deleting a ride while the device records another: the device refuses
     * (RESULT_RECORDING, its card belongs to the recorder). What the rider is
     * left with must be the truth: either the app says it couldn't delete, or
     * the ride is still listed (and still on the phone if it was downloaded).
     */
    @Ignore(
        "BUG: deleting a ride while the device records: the device refuses (RESULT_RECORDING) but " +
            "BleManager.deleteRide drops the ride from the list and deletes the phone's downloaded copy before " +
            "asking, and deleteFile's reply is ignored (default done = {}), so the rider sees it deleted with no " +
            "message; the ride is back in the list after the next check, and the phone's copy is gone " +
            "(BleManager.kt deleteRide; iOS BLEManager.swift deleteRide the same)",
    )
    @Test
    fun aRideDeletedWhileTheDeviceRecordsIsNotSilentlyLost() {
        val now = System.currentTimeMillis() / 1000 / 60 * 60
        val spec = RideSpec(start = now - 86_400L, seconds = 600)
        cardRides(listOf(spec))
        val ble = connected()
        ridesTabCheck(ble)
        downloadAndWait(ble, spec.name)
        val bytes = ble.cachedRideFile(spec.name).readBytes()

        sim.cmd("gps", "track" to lineTrack(45.70, -121.52, 3.0), "speed_kmh" to 25)
        waitFor("a GPS fix", 15_000) { rideStatus().get("gps_fix").asBoolean }
        sim.cmd("button", "name" to "boot")
        waitFor("recording, and the app knows", 15_000) { rideStatus().get("recording").asBoolean && ble.status.recording }

        val seq = sim.logSeq()
        ble.lastMessage = null
        ble.deleteRide(spec.name)
        runFor(3_000)
        assertTrue("the device kept the ride (it refuses deletes while recording)", cardRide(spec.name).exists())
        println("after the refused delete: listed=${ble.rides.map { it.name }} cached=${ble.isCached(spec.name)} " +
            "message=${ble.lastMessage} console=${sim.logSince(seq)}")
        val told = ble.lastMessage != null && ble.lastMessage!!.contains("delete", ignoreCase = true)
        val stillThere = ble.rides.any { it.name == spec.name } && ble.isCached(spec.name)
        assertTrue(
            "the device refused the delete, but the app dropped the ride from the list and deleted the phone's " +
                "copy without a word (listed=${ble.rides.map { it.name }}, cached=${ble.isCached(spec.name)}, " +
                "message=${ble.lastMessage})",
            told || stillThere,
        )
        assertArrayEquals(bytes, cardRide(spec.name).readBytes())
    }

    // ---- rides recorded while the app was away -------------------------------------------------

    /**
     * The phone is out of reach (Bluetooth off) while the rider records two
     * rides; when it is back, the Rides tab's once-per-link check lists them,
     * newest first, above the rides it already knew, and they download.
     */
    @Test
    fun ridesRecordedWhileTheAppWasAwayAppearOnReconnect() {
        val now = System.currentTimeMillis() / 1000 / 60 * 60
        val old = RideSpec(start = now - 5 * 86_400L, seconds = 300)
        cardRides(listOf(old))
        val radio = radio()
        val ble = manager(radio)
        pairFresh(ble, radio)
        ridesTabCheck(ble)
        assertEquals(listOf(old.name), ble.rides.map { it.name })

        radio.setEnabled(false)
        waitFor("the phone gone", 10_000) { !phoneConnected() && ble.state == ConnState.POWERED_OFF }
        recordRideOnDevice(seconds = 60)
        // A minute later (device clock), another.
        sim.cmd("gps", "track" to lineTrack(45.71, -121.52, 3.0), "speed_kmh" to 25)
        Thread.sleep(1_500)
        recordRideOnDevice(seconds = 60)
        val recorded = File(sd, "rides").list()!!.filter { it != old.name && it.endsWith(".fit") }.sortedDescending()
        assertEquals("two new rides on the card: $recorded", 2, recorded.size)
        assertEquals("the app still shows what it knew while away", listOf(old.name), ble.rides.map { it.name })

        radio.setEnabled(true)
        waitFor("reconnected", 30_000, neverRefused(ble)) { connectedOverLink(ble) }
        assertFalse("a new link is checked afresh", ble.ridesCheckedThisLink)
        ridesTabCheck(ble)
        assertEquals("the new rides above the old", recorded + old.name, ble.rides.map { it.name })
        downloadAndWait(ble, recorded.first())
        assertArrayEquals(cardRide(recorded.first()).readBytes(), ble.cachedRideFile(recorded.first()).readBytes())
    }

    // ---- interrupted downloads -------------------------------------------------------------------

    /** A long ride (four hours, ~350 KB) on a device with a slow link, so a drop lands mid-download. */
    private fun slowDeviceWithALongRide(): Pair<RideSpec, File> {
        sim.close()
        sim = startSim(listOf("--conn-interval-ms", "60", "--packets-per-event", "2", "--notify-window", "4"))
        val spec = RideSpec(start = System.currentTimeMillis() / 1000 / 60 * 60 - 86_400, seconds = 4 * 3_600, hr = 140, watts = 190)
        val file = cardRides(listOf(spec)).single()
        assertTrue("a long ride: ${file.length()} B", file.length() > 250_000)
        return spec to file
    }

    /**
     * The link drops part way through a long ride's download: the app says
     * the transfer failed (not stuck at "downloading"), saves nothing under
     * the ride's name, and keeps what arrived; the next tap resumes from there
     * (the device starts at that offset, not at 0) and the ride arrives whole.
     */
    @Test
    fun anInterruptedRideDownloadResumesWhereItStopped() {
        val (spec, file) = slowDeviceWithALongRide()
        val radio = radio()
        val ble = manager(radio)
        pairFresh(ble, radio)
        ridesTabCheck(ble)

        ble.downloadRide(spec.name)
        waitFor("a third of the ride downloaded", 120_000) { ble.downloadProgress >= 0.33 }
        val upBefore = radio.linksUp.get()
        sim.dropPhone()
        waitFor("the download reported as failed", 15_000) { ble.downloadingName == null }
        assertFalse("nothing saved under the ride's name", ble.isCached(spec.name))
        assertEquals("Connection dropped mid-download — try again", ble.lastMessage)
        val partial = partialFile(spec.name)
        assertTrue("what arrived is kept for a resume", partial.exists() && partial.length() > 0)
        val kept = partial.length()
        assertArrayEquals("the kept bytes are the ride's", file.readBytes().copyOf(kept.toInt()), partial.readBytes())

        waitFor("reconnected", 30_000, neverRefused(ble)) { connectedOverLink(ble) && radio.linksUp.get() > upBefore }
        val seq = sim.logSeq()
        downloadAndWait(ble, spec.name, timeoutMs = 180_000)
        assertArrayEquals("the resumed ride is whole", file.readBytes(), ble.cachedRideFile(spec.name).readBytes())
        assertFalse("the partial copy is gone", partial.exists())
        val begin = consoleSince(seq, Regex("link: download \\d+ begin: ${Regex.escape(spec.name)}"))
        assertEquals("one download: $begin", 1, begin.size)
        assertTrue("resumed from $kept, not from 0: ${begin.single()}", begin.single().endsWith("bytes from $kept, window 16384") ||
            Regex("from $kept\\b").containsMatchIn(begin.single()))
    }

    /**
     * The ride on the card changed between the interrupted download and the
     * retry (same name and size, different bytes — e.g. a card swapped in a
     * computer): the resumed bytes can't be trusted. The app must never cache
     * the mixture; it says so, and the next tap fetches the ride afresh.
     */
    @Test
    fun aResumeOverAChangedRideNeverSavesAMixture() {
        val (spec, file) = slowDeviceWithALongRide()
        val radio = radio()
        val ble = manager(radio)
        pairFresh(ble, radio)
        ridesTabCheck(ble)
        ble.downloadRide(spec.name)
        waitFor("a third of the ride downloaded", 120_000) { ble.downloadProgress >= 0.33 }
        val upBefore = radio.linksUp.get()
        sim.dropPhone()
        waitFor("the download reported as failed", 15_000) { ble.downloadingName == null }
        assertTrue(partialFile(spec.name).length() > 1000)

        // Changed on the card: its first kilobyte, which the phone already has.
        val changed = file.readBytes().also { for (i in 100 until 1100) it[i] = (it[i].toInt() xor 0x5A).toByte() }
        file.writeBytes(changed)

        waitFor("reconnected", 30_000) { connectedOverLink(ble) && radio.linksUp.get() > upBefore }
        ble.downloadRide(spec.name)
        waitFor("the first retry ended", 180_000) { ble.downloadingName == null && ble.queuedDownloads.isEmpty() }
        if (ble.isCached(spec.name)) {
            assertArrayEquals("a mixture of the old and new ride was saved", changed, ble.cachedRideFile(spec.name).readBytes())
        } else {
            assertEquals("Transfer corrupted — try again", ble.lastMessage)
            assertFalse("the bad partial copy is dropped", partialFile(spec.name).exists())
            downloadAndWait(ble, spec.name, timeoutMs = 180_000)
            assertArrayEquals("the second retry fetches it afresh", changed, ble.cachedRideFile(spec.name).readBytes())
        }
    }
}
