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
import com.raemond.opentrailpaper.map.CdnVersions
import com.raemond.opentrailpaper.map.DebugEndpoints
import com.raemond.opentrailpaper.map.MapDownloadProgress
import com.raemond.opentrailpaper.map.MapEndpoints
import com.raemond.opentrailpaper.map.MapTile
import com.raemond.opentrailpaper.map.PrebuiltTiles
import com.raemond.opentrailpaper.map.TileCache
import com.raemond.opentrailpaper.map.runMapDownload
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.fail
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import java.io.File
import java.time.Duration

/**
 * What the map sim tests (MapsSimTest, PoiSimTest) share: a factory-fresh
 * device in otp-sim, the stand-in CDN (sim/cdn/tile_cdn.py serving
 * sim/fixtures/appmaps) behind the app's debug-only endpoint override, the
 * app's real BleManager on SimRadio, and the Maps screen's download as
 * MapsSheet.startDownload runs it.
 *
 * The area is picked as the Maps screen picks it, minus the H3 cover (JNI,
 * not loadable on the JVM): the fixture lists the cells the apps' own cover
 * gives for its box (sim/cdn/fetch_fixture.py).
 */
abstract class MapSimHarness {

    protected lateinit var app: Application
    protected lateinit var sim: FirmwareSim
    protected lateinit var cdn: TileCdn
    private var radio: SimRadio? = null
    protected val scope = CoroutineScope(Dispatchers.Main + SupervisorJob())
    private val log = StringBuilder()

    protected val tiles: List<MapTile> get() = cdn.cells.map { it.tile }
    protected val ids: Set<String> get() = cdn.cells.map { it.tile.id }.toSet()

    @Before
    fun setUp() {
        if (System.getenv("OTP_SIM_REQUIRED") != null) {
            check(FirmwareSim.binary != null) { "OTP_SIM_REQUIRED, but sim/build/otp-sim is not built" }
        }
        assumeTrue("sim/build/otp-sim is not built (run sim/build.sh)", FirmwareSim.binary != null)
        assumeTrue("sim/cdn/tile_cdn.py not found", TileCdn.root != null)
        app = RuntimeEnvironment.getApplication()
        shadowOf(app).grantPermissions(Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.BLUETOOTH_SCAN)
        Prefs.onboarded = true
        TileCache.init(app)        // and PoiCache
        // Process-wide caches of the CDN's indexes: each test starts as a
        // fresh app would.
        PrebuiltTiles.IndexCache.clear()
        CdnVersions.clear()
        cdn = TileCdn.start()
        // The debug build's switch, as on an emulator (sim/ANDROID.md).
        File(app.filesDir, "otp-endpoints").writeText("tiles=${cdn.baseUrl}\noverpass=${cdn.overpassUrl}\n")
        DebugEndpoints.init(app)
        assertEquals("the app's CDN is the stand-in", cdn.baseUrl, MapEndpoints.tileBaseUrl)
        sim = FirmwareSim.start()
    }

    @After
    fun tearDown() {
        scope.cancel()
        if (::sim.isInitialized) println(dump())
        runCatching { radio?.setEnabled(false) }
        if (::sim.isInitialized) sim.close()
        if (::cdn.isInitialized) cdn.close()
    }

    protected fun connect(): BleManager {
        val store = app.getSharedPreferences("simradio-maps", Context.MODE_PRIVATE)
        val r = SimRadio(
            "127.0.0.1", sim.blePort, store,
            SimRadio.Options(
                phoneName = "Sim maps",
                directConnectTimeoutMs = 1_500,
                log = { synchronized(log) { log.append(System.currentTimeMillis() % 100_000).append(' ').append(it).append('\n') } },
            ),
        )
        radio = r
        val ble = BleManager(app, r)
        waitFor("pairing and Hello", 30_000) { ble.state == ConnState.CONNECTED && ble.linkReady && ble.deviceHello != null }
        assumeTrue("firmware without map tiles", ble.has(Feature.FEATURE_MAP_TILES))
        // What the Maps screen asks for when it opens.
        ble.refreshDeviceTiles()
        waitFor("the device's tile and POI lists", 15_000) { ble.deviceSupportsPois }
        return ble
    }

    /** The main looper, moving with the wall clock (see BleManagerSimTest.LooperClock). */
    protected fun waitFor(what: String, timeoutMs: Long, cond: () -> Boolean) {
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

    protected fun dump(): String {
        val cdnLog = runCatching { cdn.log().joinToString("\n") { "${it.get("method").asString} ${it.get("path").asString} ${it.get("status")}" } }
            .getOrDefault("")
        return "--- SimRadio ---\n${synchronized(log) { log.toString().lines().takeLast(40).joinToString("\n") }}" +
            "\n--- stand-in CDN ---\n$cdnLog" +
            "\n--- otp-sim console (tail) ---\n${sim.consoleTail(40)}"
    }

    /**
     * What MapsSheet.startDownload does for the selection [tiles]: Download
     * (new hexes, plus POIs the device lacks or the CDN has changed — the
     * "Send POIs" button when no hex is new), or Redownload (the hexes on the
     * device). Returns the status the screen would show at the end.
     */
    protected fun download(ble: BleManager, redownload: Boolean = false, timeoutMs: Long = 90_000): String? {
        val missing0 = if (redownload) tiles.filter { ble.tileIsCurrent(it.id) } else tiles.filter { !ble.tileIsCurrent(it.id) }
        val poiWork0 = when {
            !redownload -> tiles.filter { ble.poiNeedsSend(it.id) }
            ble.deviceSupportsPois -> tiles.filter { ble.tileIsCurrent(it.id) }
            else -> emptyList()
        }
        var result: String? = null
        val job: Job = scope.launch {
            result = runMapDownload(
                ble, missing0, poiWork0, redownload, force = false,
                tileRecords = ble.tileRecords, poiRecords = ble.poiRecords, devicePois = ble.devicePoiIds,
                progress = MapDownloadProgress(status = {}, total = {}, built = {}, failed = {}),
            )
        }
        waitFor("the download and its transfer", timeoutMs) { job.isCompleted && !ble.tilesUploading }
        // The device's own list again, as the app re-reads it after a send.
        ble.refreshDeviceTiles()
        runFor(1_500)
        return result
    }

    /** What the Maps screen does when it shows the selection: look its hexes
     *  up in the CDN's indexes (CdnVersions), for the version states. */
    protected fun loadCdnVersions() {
        val job = scope.launch { CdnVersions.load(ids) }
        waitFor("the CDN's indexes", 15_000) { job.isCompleted }
    }

    protected fun runFor(ms: Long) {
        val end = System.currentTimeMillis() + ms
        waitFor("($ms ms)", ms + 5_000) { System.currentTimeMillis() >= end }
    }

    protected fun fixtureBytes(id: String, ext: String) = File(cdn.fixture, "v1/${id.take(6)}/$id.$ext").readBytes()

    /** The file on the device's card (map_store's layout: /maps/tiles/<first 6>/<rest>.<ext>). */
    protected fun cardFile(id: String, ext: String) = File(sim.dir, "sd/maps/tiles/${id.take(6)}/${id.drop(6)}.$ext")

    protected fun tileGets(paths: List<String>) = paths.filter { it.endsWith(".ebm") }
    protected fun poiGets(paths: List<String>) = paths.filter { it.endsWith(".poi") }
    protected fun overpass(paths: List<String>) = paths.filter { it.endsWith("/api/interpreter") }

    /** Tiles and POI files the device saved since [seq] (its "tile saved:" log). */
    protected fun savedSince(seq: Long): List<String> =
        sim.logSince(seq).mapNotNull { Regex("tile saved: (\\S+)").find(it)?.groupValues?.get(1) }

    protected fun assertNoInternet(paths: List<String>) {
        // Every request the run made reached the stand-in; it has no Overpass
        // data, so a selection inside the fixture must never ask for it.
        assertEquals("Overpass was asked: $paths", emptyList<String>(), overpass(paths))
    }

    // ---- the device's panel ------------------------------------------------------------------

    /** The device's map screen, freshly drawn (from another screen, so the
     *  whole map is painted again), as a screenshot: width and gray pixels. */
    protected fun mapScreenshot(name: String): Pair<Int, ByteArray> {
        sim.cmd("console", "line" to "screen dash", "wait_ms" to 300)
        sim.cmd("console", "line" to "screen map", "wait_ms" to 500)
        waitFor("the map screen", 10_000) { sim.status().get("screen").asString == "map" }
        runFor(2_500)   // let it draw the ground around the fix
        val out = File(System.getProperty("java.io.tmpdir"), "otp-maps-${System.nanoTime()}-$name.png")
        sim.cmd("screenshot", "path" to out.path)
        println("screenshot $name: $out")
        return grayPng(out.readBytes())
    }

    /**
     * Dark pixels on the device's map screen, away from the fixed furniture
     * (status bar, compass, zoom buttons, the "NO MAP HERE" box, the position
     * marker, the scale and the data fields): roads, water and parks.
     */
    protected fun screenshotOfMap(name: String): Int {
        val (w, px) = mapScreenshot(name)
        var dark = 0
        for (y in 70 until 745) for (x in 0 until 455) {          // above the scale bar
            if (y in 395..505) continue                       // the "NO MAP HERE" box and the marker
            if (y < 150 && x > 440) continue
            if ((px[y * w + x].toInt() and 0xFF) < 100) dark++
        }
        println("screenshot $name: $dark dark pixels")
        return dark
    }

    /** The sim's screenshots: 8-bit grayscale PNG, filter 0 on every row
     *  (sim/host/epd_sim.cpp). Returns the width and the pixels. */
    protected fun grayPng(b: ByteArray): Pair<Int, ByteArray> {
        fun be32(i: Int) = ((b[i].toInt() and 0xFF) shl 24) or ((b[i + 1].toInt() and 0xFF) shl 16) or
            ((b[i + 2].toInt() and 0xFF) shl 8) or (b[i + 3].toInt() and 0xFF)
        var i = 8
        var w = 0
        var h = 0
        val idat = java.io.ByteArrayOutputStream()
        while (i + 8 <= b.size) {
            val len = be32(i)
            val type = String(b, i + 4, 4, Charsets.US_ASCII)
            if (type == "IHDR") { w = be32(i + 8); h = be32(i + 12) }
            if (type == "IDAT") idat.write(b, i + 8, len)
            i += 12 + len
        }
        val raw = java.util.zip.InflaterInputStream(idat.toByteArray().inputStream()).readBytes()
        val px = ByteArray(w * h)
        for (y in 0 until h) System.arraycopy(raw, y * (w + 1) + 1, px, y * w, w)
        return w to px
    }
}
