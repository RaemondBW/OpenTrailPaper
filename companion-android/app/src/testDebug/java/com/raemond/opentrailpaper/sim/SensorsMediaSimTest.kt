package com.raemond.opentrailpaper.sim

import android.Manifest
import android.app.Application
import android.content.Context
import android.os.Looper
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.ble.BleManager.ConnState
import com.raemond.opentrailpaper.ble.SimRadio
import com.raemond.opentrailpaper.data.Prefs
import com.raemond.opentrailpaper.link.proto.Feature
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
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

/**
 * The app's Sensors sheet and music feed, through the real BleManager, against
 * the firmware sim: the sim's virtual sensors (sim/PROTOCOL.md `sensor`) are
 * found by the app's scan, paired and forgotten from it; the app's now-playing
 * metadata and album art (dithered on the phone to tone indices when the
 * device takes them, ArtDither) land in the device's media store
 * (control `media_state`).
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = SimTestApp::class)
class SensorsMediaSimTest {

    private lateinit var app: Application
    private lateinit var sim: FirmwareSim
    private val radios = mutableListOf<SimRadio>()
    private val log = StringBuilder()

    @Before
    fun setUp() {
        if (System.getenv("OTP_SIM_REQUIRED") != null) {
            check(FirmwareSim.binary != null) { "OTP_SIM_REQUIRED, but sim/build/otp-sim is not built" }
        }
        assumeTrue("sim/build/otp-sim is not built (run sim/build.sh)", FirmwareSim.binary != null)
        app = RuntimeEnvironment.getApplication()
        shadowOf(app).grantPermissions(Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.BLUETOOTH_SCAN)
        Prefs.onboarded = true
        sim = FirmwareSim.start()
    }

    @After
    fun tearDown() {
        if (::sim.isInitialized) println(dump())
        radios.forEach { runCatching { it.setEnabled(false) } }
        if (::sim.isInitialized) sim.close()
    }

    // ---- harness (as BleManagerSimTest) ----------------------------------------------------

    private fun radio(): SimRadio {
        val store = app.getSharedPreferences("simradio-phoneA", Context.MODE_PRIVATE)
        return SimRadio(
            "127.0.0.1", sim.blePort, store,
            SimRadio.Options(
                phoneName = "Sim phoneA",
                directConnectTimeoutMs = 1_500,
                log = { synchronized(log) { log.append(System.currentTimeMillis() % 100_000).append(' ').append(it).append('\n') } },
            ),
        ).also { radios += it }
    }

    private class LooperClock {
        private var last = System.nanoTime()
        fun advance() {
            val now = System.nanoTime()
            val ms = maxOf(1L, (now - last) / 1_000_000)
            last = now
            shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(ms))
        }
    }

    private fun waitFor(what: String, timeoutMs: Long, cond: () -> Boolean) {
        val end = System.currentTimeMillis() + timeoutMs
        val clock = LooperClock()
        while (true) {
            clock.advance()
            if (cond()) return
            if (System.currentTimeMillis() > end) fail("timed out waiting for $what\n${dump()}")
            Thread.sleep(20)
        }
    }

    private fun runFor(ms: Long) {
        val end = System.currentTimeMillis() + ms
        val clock = LooperClock()
        while (System.currentTimeMillis() < end) {
            clock.advance()
            Thread.sleep(20)
        }
    }

    private fun dump(): String {
        val tail = runCatching { sim.consoleLog.readLines().takeLast(30).joinToString("\n") }.getOrDefault("")
        return "--- SimRadio ---\n${synchronized(log) { log.toString().lines().takeLast(60).joinToString("\n") }}" +
            "\n--- otp-sim console (tail) ---\n$tail"
    }

    private fun connected(): BleManager {
        val ble = BleManager(app, radio())
        waitFor("connect, pair and Hello", 30_000) {
            ble.state == ConnState.CONNECTED && ble.linkReady && ble.deviceHello != null
        }
        return ble
    }

    private fun sensorState() = sim.cmd("sensor_state").getAsJsonObject("state")
    private fun media() = sim.cmd("media_state").getAsJsonObject("media")

    // ---- sensors ------------------------------------------------------------------------------

    /**
     * The Sensors sheet: its scan lists the strap by name, Pair connects it
     * (the list then shows it connected, and the device's heart rate is the
     * strap's), Forget drops it and it stays dropped.
     */
    @Test
    fun sensorsSheetScansPairsAndForgetsAStrap() {
        val ble = connected()
        assertTrue(ble.has(Feature.FEATURE_SENSORS))
        val addr = sim.cmd("sensor", "kind" to "hr", "bpm" to 137, "name" to "HRM-Pro").get("addr").asString
        ble.startSensorScan()
        waitFor("the scan lists the strap", 15_000) { ble.sensors.any { it.addr == addr && it.name == "HRM-Pro" } }
        val seen = ble.sensors.first { it.addr == addr }
        assertEquals("listed as a heart-rate sensor", 1, seen.kindsMask)
        assertFalse(seen.paired)

        ble.pairSensor(addr)
        ble.stopSensorScan()
        waitFor("the strap connected", 25_000) {
            ble.refreshSensors()
            runFor(200)
            ble.sensors.any { it.addr == addr && it.paired && it.connected }
        }
        assertEquals(addr, ble.connectedSensor(1)?.addr)
        assertEquals(137, sensorState().get("hr_bpm").asInt)

        ble.forgetSensor(addr)
        waitFor("the strap dropped", 10_000) {
            !sensorState().get("hr_connected").asBoolean && ble.sensors.none { it.addr == addr && it.paired }
        }
        runFor(5_000)
        assertFalse("a forgotten strap reconnected", sensorState().get("hr_connected").asBoolean)
        // The FORGET answer itself is stale (see the ignored test below): a
        // fresh list, as reopening the sheet asks for, has it gone.
        ble.refreshSensors()
        waitFor("a fresh list without the strap connected", 5_000) { ble.connectedSensor(1) == null }
    }

    /** What the rider sees straight after Forget, without reopening the sheet. */
    @Ignore(
        "BUG: the device answers FORGET (src/link.cpp onSensors) before the strap's link has " +
            "dropped, so the list says it is still connected, and no later list comes with the scan " +
            "stopped: the Sensors sheet keeps showing a forgotten strap as 'Connected'",
    )
    @Test
    fun forgottenStrapLeavesTheSheetWithoutARefresh() {
        val ble = connected()
        val addr = sim.cmd("sensor", "kind" to "hr", "bpm" to 120).get("addr").asString
        ble.startSensorScan()
        waitFor("the scan lists the strap", 15_000) { ble.sensors.any { it.addr == addr } }
        ble.pairSensor(addr)
        ble.stopSensorScan()
        waitFor("the strap connected", 25_000) {
            ble.refreshSensors()
            runFor(200)
            ble.connectedSensor(1)?.addr == addr
        }
        ble.forgetSensor(addr)
        runFor(5_000)
        assertEquals("the sheet still shows the forgotten strap connected", null, ble.connectedSensor(1))
    }

    // ---- music ------------------------------------------------------------------------------

    /**
     * Android has no AMS: the app is the music page's only feed. Its track,
     * then its album art (tone-dithered on the phone when the device says
     * FEATURE_MEDIA_ART_TONE) reach the device; a clear empties the page.
     */
    @Test
    fun nowPlayingAndAlbumArtFromTheAppReachTheDevice() {
        val ble = connected()
        ble.sendMediaMeta(true, 42, 215, "Dreams", "Fleetwood Mac", "Rumours")
        waitFor("the track on the device", 10_000) { media().get("title").asString == "Dreams" }
        val m = media()
        assertEquals("Fleetwood Mac", m.get("artist").asString)
        assertEquals(215, m.get("duration_s").asInt)
        assertTrue(m.get("playing").asBoolean)
        assertFalse("not AMS: this is the app's feed", m.get("ams").asBoolean)

        val w = 120
        val h = 120
        val gray = ByteArray(w * h) { i -> ((i % w) * 255 / (w - 1)).toByte() }
        ble.sendMediaArt(gray, w, h)
        waitFor("the cover on the device", 20_000) { media().get("art_w").asInt == w }
        val art = media()
        assertEquals(h, art.get("art_h").asInt)
        val tones = art.getAsJsonArray("art_tones").map { it.asInt }
        assertEquals("every pixel is one of the panel's tones", w * h, tones.sum())
        assertTrue("a ramp uses all five tones: $tones", tones.all { it > 0 })

        ble.sendMediaClear()
        waitFor("the page emptied", 10_000) { !media().get("present").asBoolean }
        assertEquals(0, media().get("art_w").asInt)
    }
}
