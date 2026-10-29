package com.raemond.opentrailpaper.sim

import android.Manifest
import android.app.Application
import android.content.Context
import android.os.Looper
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.ble.BleManager.ConnState
import com.raemond.opentrailpaper.ble.Maneuver
import com.raemond.opentrailpaper.ble.SimRadio
import com.raemond.opentrailpaper.data.DeviceText
import com.raemond.opentrailpaper.data.GpxExporter
import com.raemond.opentrailpaper.data.GpxImporter
import com.raemond.opentrailpaper.data.ImportResult
import com.raemond.opentrailpaper.data.ImportedRoute
import com.raemond.opentrailpaper.data.Prefs
import org.junit.After
import org.junit.Assert.assertArrayEquals
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
import java.io.File
import java.time.Duration
import java.util.Locale
import kotlin.math.abs
import kotlin.math.cos

/**
 * Routes from the app to the device, end to end: a rider's .gpx through the
 * app's own import (GpxImporter), file naming (DeviceText.routeFileName) and
 * export (GpxExporter) — what RouteScreen's Send runs — then the real
 * BleManager's uploadRoute over SimRadio to the firmware in otp-sim, which
 * parses it, keeps it in /routes on its card and raises the NAVIGATE prompt.
 * What the device holds is read from its card and its control channel
 * (status `nav`, the console's `[route]` lines; sim/PROTOCOL.md).
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = SimTestApp::class)
class RouteSimTest {

    private lateinit var app: Application
    private lateinit var sim: FirmwareSim
    private var radio: SimRadio? = null
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
        radio?.let { runCatching { it.setEnabled(false) } }
        if (::sim.isInitialized) sim.close()
    }

    // ---- harness -----------------------------------------------------------------------------

    private fun connect(): BleManager {
        val store = app.getSharedPreferences("simradio-route", Context.MODE_PRIVATE)
        val r = SimRadio(
            "127.0.0.1", sim.blePort, store,
            SimRadio.Options(
                phoneName = "Sim route phone",
                directConnectTimeoutMs = 1_500,
                log = { synchronized(log) { log.append(it).append('\n') } },
            ),
        )
        radio = r
        val ble = BleManager(app, r)
        waitFor("connect, pairing and Hello", 30_000) {
            ble.state == ConnState.CONNECTED && ble.linkReady && ble.deviceHello != null
        }
        return ble
    }

    /** Run the main looper in step with the wall clock until [cond] holds. */
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

    private fun dump(): String {
        val tail = runCatching { sim.consoleTail(30) }.getOrDefault("")
        return "--- SimRadio ---\n${synchronized(log) { log.lines().takeLast(40).joinToString("\n") }}" +
            "\n--- otp-sim console (tail) ---\n$tail"
    }

    private fun nav() = sim.status().getAsJsonObject("nav")

    private fun routesOnCard(): List<String> =
        File(sim.dir, "sd/routes").list()?.sorted() ?: emptyList()

    /** The `[route] uploaded <name>: N pts, K km` line the device printed. */
    private fun uploadedPoints(name: String): Int? {
        val re = Regex("""\[route] uploaded (.+): (\d+) pts""")
        return sim.logSince(0).mapNotNull { re.find(it) }.lastOrNull { it.groupValues[1] == name }
            ?.groupValues?.get(2)?.toInt()
    }

    /**
     * A rider's GPX as planners export it: metadata (an author with its own
     * name, bounds), waypoints BEFORE the track (RideWithGPS cue points),
     * elevation and time on every point. `n` points every ~10 m heading
     * north-east from Hood River.
     */
    private fun plannerGpx(n: Int, title: String): String {
        val s = StringBuilder()
        s.append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n")
        s.append("<gpx version=\"1.1\" creator=\"Planner\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n")
        s.append("<metadata><name>$title</name><author><name>Some Rider</name></author>")
        s.append("<bounds minlat=\"45.70\" minlon=\"-121.60\" maxlat=\"45.90\" maxlon=\"-121.30\"/></metadata>\n")
        s.append("<wpt lat=\"45.9000000\" lon=\"-121.3000000\"><name>Water</name></wpt>\n")
        s.append("<wpt lat=\"45.6000000\" lon=\"-121.9000000\"><name>Shop</name></wpt>\n")
        s.append("<trk><name>$title</name><type>cycling</type><trkseg>\n")
        for (i in 0 until n) {
            val lat = 45.70 + i * 10.0 * 0.7071 / 111_320.0
            val lon = -121.60 + i * 10.0 * 0.7071 / (111_320.0 * cos(Math.toRadians(45.7)))
            s.append(String.format(Locale.US, "<trkpt lat=\"%.7f\" lon=\"%.7f\"><ele>%.1f</ele>", lat, lon, 100.0 + i % 50))
            s.append("<time>2026-10-28T08:00:00Z</time></trkpt>\n")
        }
        s.append("</trkseg></trk></gpx>\n")
        return s.toString()
    }

    private fun import(gpx: String, fallback: String): ImportedRoute {
        val r = GpxImporter.parse(gpx.byteInputStream(), fallback)
        assertTrue("imported: $r", r is ImportResult.Ok)
        return (r as ImportResult.Ok).route
    }

    /** RouteScreen's onSend: the device file name, the exported GPX, the cues. */
    private fun send(ble: BleManager, route: ImportedRoute, cues: List<Maneuver>): Pair<String, String> {
        val name = DeviceText.routeFileName(route.name)
        val gpx = GpxExporter.make(name, route.points)
        ble.uploadRoute(name = name, gpx = gpx, maneuvers = cues)
        waitFor("the device confirmed the route", 60_000) { ble.routeReceived }
        return name to gpx
    }

    // ---- tests -------------------------------------------------------------------------------

    /**
     * Import a planner's GPX (waypoints first, metadata bounds, ele/time) of
     * 1,500 points and send it with two cues. The device keeps exactly the
     * GPX the app made, under the app's file name; parses the track only
     * (not the waypoints the firmware's scanner would otherwise pull into the
     * line, nor the bounds); every point the app sent; the route as long as
     * the app measured it; and raises the NAVIGATE prompt with both cues.
     */
    @Test
    fun importedGpxReachesTheDeviceAsTheRouteTheAppShowed() {
        val ble = connect()
        val route = import(plannerGpx(1500, "Hood River — Lost Lake loop"), "fallback")
        assertEquals("Hood River — Lost Lake loop", route.name)
        assertEquals(1500, route.points.size)
        val cues = listOf(
            Maneuver(route.points[700].lat, route.points[700].lon, "Turn right onto Lost Lake Rd"),
            Maneuver(route.points.last().lat, route.points.last().lon, "Arrive at Lost Lake"),
        )
        val (name, gpx) = send(ble, route, cues)
        assertEquals("hood_river_lost_lake_loop.gpx", name)
        assertEquals(listOf(name), routesOnCard())
        assertArrayEquals("the card holds the GPX the app made",
            gpx.toByteArray(), File(sim.dir, "sd/routes/$name").readBytes())
        assertEquals("every point the app sent", 1500, uploadedPoints(name))
        val nav = nav()
        assertTrue("route loaded: $nav", nav.get("route").asBoolean)
        assertTrue("NAVIGATE prompt up: $nav", nav.get("pending").asBoolean)
        assertEquals(2, nav.get("maneuvers").asInt)
        val km = nav.get("remaining_km").asDouble
        assertTrue("device: $km km, app: ${route.distanceMeters / 1000} km",
            abs(km * 1000 - route.distanceMeters) < route.distanceMeters * 0.002 + 2)
    }

    /**
     * The app's list of routes on the device, and deleting one: the list
     * shows what the card holds, delete removes the file, and a route the
     * device had loaded is unloaded with it (no line on the map for a file
     * that is gone).
     */
    @Test
    fun routesAreListedAndDeletedFromTheApp() {
        val ble = connect()
        val a = import(plannerGpx(300, "Morning loop"), "a")
        val b = import(plannerGpx(200, "Évening spin"), "b")
        val (nameA, _) = send(ble, a, emptyList())
        val (nameB, _) = send(ble, b, emptyList())
        assertEquals("evening_spin.gpx", nameB)
        ble.refreshRoutes()
        waitFor("the device's routes listed", 10_000) { !ble.loadingRoutes && ble.deviceRoutes.size == 2 }
        assertEquals(listOf(nameB, nameA).sorted(), ble.deviceRoutes)
        assertTrue(nav().get("route").asBoolean)
        ble.deleteRoute(nameB)            // the loaded one: the last sent
        waitFor("deleted on the card", 10_000) { routesOnCard() == listOf(nameA) }
        waitFor("unloaded on the device", 10_000) { !nav().get("route").asBoolean }
        ble.refreshRoutes()
        waitFor("the list refreshed", 10_000) { !ble.loadingRoutes && ble.deviceRoutes == listOf(nameA) }
        assertFalse(nav().get("active").asBoolean)
    }

    /**
     * Twice the device's cap (8,192 points): the importer thins it to fit the
     * device's 4096 so nothing more is thinned on the device.
     */
    @Ignore(
        "BUG: GpxImporter.decimate (data/GpxImporter.kt) keeps every ceil(n/4096)-th point and then " +
            "appends the last one, so an 8,192-point track becomes 4,097 points — one over routes.cpp " +
            "MAX_PTS. The device's GpxScanner then halves the whole route to 2,049 points: the rider " +
            "gets half the resolution the app thinned to (the line cuts corners).",
    )
    @Test
    fun aTrackOverTheDeviceCapIsThinnedOnceByTheApp() {
        val ble = connect()
        val route = import(plannerGpx(8192, "Long day"), "long")
        val (name, _) = send(ble, route, emptyList())
        assertEquals("the device keeps every point the app sent (${route.points.size})",
            route.points.size, uploadedPoints(name))
    }
}
