package com.raemond.opentrailpaper.sim

import android.Manifest
import android.app.Application
import android.content.Context
import android.os.Looper
import com.google.gson.JsonArray
import com.google.gson.JsonObject
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.ble.BleManager.ConnState
import com.raemond.opentrailpaper.ble.BleManager.PairingIssue
import com.raemond.opentrailpaper.ble.SimRadio
import com.raemond.opentrailpaper.data.Prefs
import org.junit.After
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import java.io.File
import java.time.Duration
import kotlin.math.cos
import kotlin.math.sin

/**
 * What the app-flow sim tests (RideSyncSimTest, LinkEdgeSimTest) share: a
 * factory-fresh device in otp-sim (optionally with extra options, e.g. a slow
 * link), the app's real BleManager on SimRadio, the Robolectric main looper
 * driven in step with the wall clock, and the rider's side of the device:
 * rides recorded on it, FIT files the firmware's own FitWriter writes onto its
 * card (`otp-sim card-tool fit`), its console and status.
 *
 * Kept apart from BleManagerSimTest's private copy so the two can change
 * independently.
 */
abstract class AppSimHarness {

    protected lateinit var app: Application
    protected lateinit var sim: FirmwareSim
    protected val radios = mutableListOf<SimRadio>()
    private val log = StringBuilder()

    /** Extra otp-sim options for this class's device. */
    protected open val simArgs: List<String> = emptyList()

    @Before
    fun setUpSim() {
        if (System.getenv("OTP_SIM_REQUIRED") != null) {
            check(FirmwareSim.binary != null) { "OTP_SIM_REQUIRED, but sim/build/otp-sim is not built" }
        }
        assumeTrue("sim/build/otp-sim is not built (run sim/build.sh)", FirmwareSim.binary != null)
        app = RuntimeEnvironment.getApplication()
        shadowOf(app).grantPermissions(Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.BLUETOOTH_SCAN)
        Prefs.onboarded = true
        sim = startSim(simArgs)
    }

    /** A device with [args]; tests that need a different link than the class's start their own. */
    protected open fun startSim(args: List<String>): FirmwareSim = FirmwareSim.start(args = args).also {
        // The device powers itself off after 10 idle minutes; a long test must not race that.
        it.cmd("console", "line" to "autosleep off")
    }

    @After
    fun tearDownSim() {
        if (::sim.isInitialized) println(dump())
        radios.forEach { runCatching { it.setEnabled(false) } }
        if (::sim.isInitialized) sim.close()
    }

    // ---- the phone ---------------------------------------------------------------------------

    /** A phone: its own bond store (identity + bonds) and SimRadio options. */
    protected fun radio(phone: String = "phoneA", mtu: Int = 517, autoConnectFailMs: Long = 0): SimRadio {
        val store = app.getSharedPreferences("simradio-$phone", Context.MODE_PRIVATE)
        return SimRadio(
            "127.0.0.1", sim.blePort, store,
            SimRadio.Options(
                phoneName = "Sim $phone",
                mtu = mtu,
                autoConnectFailMs = autoConnectFailMs,
                directConnectTimeoutMs = 1_500,
                log = { synchronized(log) { log.append(System.currentTimeMillis() % 100_000).append(' ').append(it).append('\n') } },
            ),
        ).also { radios += it }
    }

    protected fun manager(radio: SimRadio) = BleManager(app, radio)

    /** The SimRadio log so far (tests read it for what the link did). */
    protected fun radioLog(): String = synchronized(log) { log.toString() }

    /**
     * Run the main looper with the clock moving as the wall clock does, until
     * [cond] holds. [every] runs on each step (for things that must never
     * happen on the way).
     */
    protected fun waitFor(what: String, timeoutMs: Long, every: () -> Unit = {}, cond: () -> Boolean) {
        val end = System.currentTimeMillis() + (timeoutMs * TIME_SCALE).toLong()
        val clock = LooperClock()
        while (true) {
            clock.advance()
            every()
            if (cond()) return
            if (System.currentTimeMillis() > end) fail("timed out waiting for $what\n${dump()}")
            Thread.sleep(STEP_MS)
        }
    }

    /** Keep the looper running for [ms], checking [every] throughout. */
    protected fun runFor(ms: Long, every: () -> Unit = {}) {
        val end = System.currentTimeMillis() + ms
        val clock = LooperClock()
        while (System.currentTimeMillis() < end) {
            clock.advance()
            every()
            Thread.sleep(STEP_MS)
        }
    }

    /** Moves the (paused) main looper by the real time since the last step. */
    private class LooperClock {
        private var last = System.nanoTime()
        fun advance() {
            val now = System.nanoTime()
            val ms = maxOf(1L, (now - last) / 1_000_000)
            last = now
            shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms))
        }
    }

    protected fun dump(): String {
        val tail = runCatching { sim.consoleLog.readLines().takeLast(40).joinToString("\n") }.getOrDefault("")
        return "--- SimRadio ---\n${synchronized(log) { log.toString().lines().takeLast(60).joinToString("\n") }}" +
            "\n--- otp-sim console (tail) ---\n$tail"
    }

    protected fun phoneConnected() = runCatching { sim.status().get("phone_connected").asBoolean }.getOrDefault(false)

    protected fun connectedOverLink(ble: BleManager) =
        ble.state == ConnState.CONNECTED && ble.linkReady && ble.deviceHello != null

    /** First connect: scan, connect, pair (the device shows the code; confirmed), Hello. */
    protected fun pairFresh(ble: BleManager, radio: SimRadio) {
        waitFor("first connect, pairing and Hello", 30_000) { connectedOverLink(ble) }
        assertNull("no pairing issue", ble.pairingIssue)
        assertTrue("Android holds the bond", radio.holdsBond(radio.deviceAddress))
        waitFor("device reports the phone connected", 5_000) { phoneConnected() }
    }

    protected val neverRefused: (BleManager) -> () -> Unit = { ble ->
        {
            val issue = ble.pairingIssue
            if (issue == PairingIssue.NOT_RECOGNISED || issue == PairingIssue.PAIRED_ELSEWHERE || issue == PairingIssue.REFUSED) {
                fail("the app told the rider the device won't have this phone ($issue)\n${dump()}")
            }
        }
    }

    /**
     * The Rides tab, as RidesScreen's LaunchedEffect runs it: once per link,
     * when connected and not already checking, ask the device for its list;
     * then wait for the answer.
     */
    protected fun ridesTabCheck(ble: BleManager, timeoutMs: Long = 30_000) {
        waitFor("connected for the Rides tab", 30_000) { connectedOverLink(ble) }
        if (!ble.ridesCheckedThisLink && !ble.loadingRides) ble.refreshRides()
        waitFor("the Rides tab's check of the device", timeoutMs) { ble.ridesCheckedThisLink && !ble.loadingRides }
    }

    // ---- the device's card ---------------------------------------------------------------------

    protected val sd: File get() = File(sim.dir, "sd")

    protected fun sdFile(dir: String, name: String, bytes: ByteArray): ByteArray {
        val d = File(sd, dir).apply { mkdirs() }
        File(d, name).writeBytes(bytes)
        return bytes
    }

    protected fun cardRide(name: String): File = File(sd, "rides/$name")

    /** One ride for [cardRides]: its start, length, place and the rider's numbers. */
    data class RideSpec(
        val start: Long,
        val seconds: Int,
        val lat: Double = 45.7,
        val lon: Double = -121.5,
        val bearing: Int = 60,
        val kmh: Double = 25.0,
        val hr: Int = 0,
        val watts: Int = 0,
    ) {
        val name: String
            get() = java.text.SimpleDateFormat("yyyyMMdd-HHmmss", java.util.Locale.US)
                .apply { timeZone = java.util.TimeZone.getTimeZone("UTC") }
                .format(java.util.Date(start * 1000)) + ".fit"
    }

    /**
     * Finished rides on the card, written by the firmware's own FitWriter
     * (`otp-sim card-tool fit`, as sim/client/otpsim/cards.py builds them).
     * The device lists its card afresh on each request, so they need no reboot.
     */
    protected fun cardRides(specs: List<RideSpec>): List<File> {
        val bin = FirmwareSim.binary ?: error("otp-sim not built")
        val p = ProcessBuilder(bin.path, "card-tool", "fit", sd.path).redirectErrorStream(true).start()
        p.outputStream.bufferedWriter().use { w ->
            for (s in specs) {
                w.write(
                    String.format(
                        java.util.Locale.US, "/rides/%s %d %d %.5f %.5f %d %.1f %d %d\n",
                        s.name, s.start, s.seconds, s.lat, s.lon, s.bearing, s.kmh, s.hr, s.watts,
                    ),
                )
            }
        }
        val out = p.inputStream.bufferedReader().readText()
        check(p.waitFor() == 0) { "card-tool fit: $out" }
        return specs.map { cardRide(it.name).also { f -> check(f.exists()) { "card-tool didn't write ${f.name}: $out" } } }
    }

    // ---- riding on the device ------------------------------------------------------------------

    /** Points along a straight line from (lat, lon), every [stepM] metres. */
    protected fun lineTrack(lat: Double, lon: Double, km: Double, bearingDeg: Double = 90.0, stepM: Double = 50.0): JsonArray {
        val r = Math.toRadians(bearingDeg)
        val out = JsonArray()
        var d = 0.0
        while (d <= km * 1000) {
            val p = JsonArray()
            p.add(lat + d * cos(r) / 111_320.0)
            p.add(lon + d * sin(r) / (111_320.0 * cos(Math.toRadians(lat))))
            out.add(p)
            d += stepM
        }
        return out
    }

    /** The device's state of its ride (`status.ride`). */
    protected fun rideStatus(): JsonObject = sim.status().getAsJsonObject("ride")

    /** Polls the device's status (no looper: nothing on the phone side waits on it). */
    protected fun waitDevice(what: String, timeoutMs: Long, pred: (JsonObject) -> Boolean): JsonObject {
        val end = System.currentTimeMillis() + (timeoutMs * TIME_SCALE).toLong()
        while (true) {
            val st = sim.status()
            if (pred(st)) return st
            if (System.currentTimeMillis() > end) fail("timed out waiting for the device: $what\n${dump()}")
            Thread.sleep(100)
        }
    }

    /**
     * The rider records a ride on the device: a GPS track at [kmh], the BOOT
     * key to start, [seconds] of device time (the device clock runs 6x),
     * BOOT to stop and SAVE on the summary (at real time). [pump] keeps the
     * phone side running meanwhile (the looper, when an app is connected).
     * Returns the device's ride state just before SAVE: what it saves, give
     * or take the moment between the two.
     */
    protected fun recordRideOnDevice(seconds: Int = 90, kmh: Double = 30.0, pump: (Long) -> Unit = { Thread.sleep(it) }): JsonObject {
        sim.cmd("gps", "track" to lineTrack(45.70, -121.52, 3.0), "speed_kmh" to kmh)
        waitUntil("a GPS fix", 15_000, pump) { rideStatus().get("gps_fix").asBoolean }
        sim.cmd("speed", "factor" to 6)
        try {
            sim.cmd("button", "name" to "boot")
            waitUntil("recording", 10_000, pump) { rideStatus().get("recording").asBoolean }
            waitUntil("$seconds s of ride", (seconds * 1000L) / 3 + 20_000, pump) {
                rideStatus().get("elapsed_s").asInt >= seconds
            }
            // Real time again for the stop: the summary is a snapshot taken at
            // the BOOT press, and the ride goes on recording until SAVE (the
            // rider may resume), so the seconds between the two are in the file.
            sim.cmd("speed", "factor" to 1)
            sim.cmd("button", "name" to "boot")    // stop: the save / discard summary
            waitUntil("the ride summary", 10_000, pump) { sim.status().get("screen").asString == "other" }
            val summary = rideStatus()
            sim.cmd("tap", "x" to SAVE_X, "y" to SAVE_Y)
            waitUntil("the ride saved", 20_000, pump) { !rideStatus().get("recording").asBoolean }
            return summary
        } finally {
            sim.cmd("speed", "factor" to 1)
        }
    }

    private fun waitUntil(what: String, timeoutMs: Long, pump: (Long) -> Unit, cond: () -> Boolean) {
        val end = System.currentTimeMillis() + (timeoutMs * TIME_SCALE).toLong()
        while (!cond()) {
            if (System.currentTimeMillis() > end) fail("timed out waiting for $what\n${dump()}")
            pump(100)
        }
    }

    /** Lines of the device's console written since [seq] that match [re]. */
    protected fun consoleSince(seq: Long, re: Regex): List<String> = sim.logSince(seq).filter { re.containsMatchIn(it) }

    companion object {
        /**
         * Wall-clock bounds are set on a developer Mac; a slower CI runner
         * scales them (OTP_SIM_TIME_SCALE, as the firmware suite does).
         */
        val TIME_SCALE: Double = System.getenv("OTP_SIM_TIME_SCALE")?.toDoubleOrNull() ?: 1.0

        /**
         * The looper step. Every GATT write waits for its callback on the main
         * looper (GattQueue), so the step bounds the phone's write rate: at
         * MTU 23 an upload is two writes per 16-byte chunk.
         */
        const val STEP_MS = 5L

        /** ui_render.cpp kSaveButton's centre on the ride summary. */
        const val SAVE_X = 24 + 120
        const val SAVE_Y = 700 + 48
    }
}
