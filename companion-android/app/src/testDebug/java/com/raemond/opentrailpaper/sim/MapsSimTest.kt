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
import com.raemond.opentrailpaper.map.PoiCache
import com.raemond.opentrailpaper.map.PrebuiltTiles
import com.raemond.opentrailpaper.map.TileCache
import com.raemond.opentrailpaper.map.runMapDownload
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Assume.assumeTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import java.io.File
import java.time.Duration

/**
 * The map flow end to end: the app's real download pipeline (runMapDownload —
 * what the Maps screen's Download / Redownload buttons run: CDN index, tile
 * and POI caches, PrebuiltTiles, Overpass fallback) fetches an area from the
 * stand-in CDN (sim/cdn/tile_cdn.py, serving the real tiles in
 * sim/fixtures/appmaps), and the real BleManager sends it to the firmware in
 * the simulator over SimRadio. What the device then holds is read from the
 * device itself (control `tiles`: the list and version records it would give
 * any phone) and from its card; what it draws, from a screenshot.
 *
 * The CDN and Overpass URLs reach the app through the debug-only override
 * (files/otp-endpoints, DebugEndpoints). Nothing here touches the network:
 * Overpass is the stand-in too, answering 503.
 *
 * The area is picked as the Maps screen picks it, minus the H3 cover (JNI,
 * not loadable on the JVM): the fixture lists the cells the apps' own cover
 * gives for its box (sim/cdn/fetch_fixture.py).
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = SimTestApp::class)
class MapsSimTest {

    private lateinit var app: Application
    private lateinit var sim: FirmwareSim
    private lateinit var cdn: TileCdn
    private var radio: SimRadio? = null
    private val scope = CoroutineScope(Dispatchers.Main + SupervisorJob())
    private val log = StringBuilder()

    private val tiles: List<MapTile> get() = cdn.cells.map { it.tile }
    private val ids: Set<String> get() = cdn.cells.map { it.tile.id }.toSet()

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

    // ---- harness -----------------------------------------------------------------------------

    private fun connect(): BleManager {
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
        val cdnLog = runCatching { cdn.log().joinToString("\n") { "${it.get("method").asString} ${it.get("path").asString} ${it.get("status")}" } }
            .getOrDefault("")
        return "--- SimRadio ---\n${synchronized(log) { log.toString().lines().takeLast(40).joinToString("\n") }}" +
            "\n--- stand-in CDN ---\n$cdnLog" +
            "\n--- otp-sim console (tail) ---\n${sim.consoleTail(40)}"
    }

    /**
     * What MapsSheet.startDownload does for the selection [tiles]: Download
     * (new hexes, plus POIs the device lacks), or Redownload (the hexes on the
     * device). Returns the status the screen would show at the end.
     */
    private fun download(ble: BleManager, redownload: Boolean = false, timeoutMs: Long = 90_000): String? {
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

    private fun runFor(ms: Long) {
        val end = System.currentTimeMillis() + ms
        waitFor("($ms ms)", ms + 5_000) { System.currentTimeMillis() >= end }
    }

    private fun fixtureBytes(id: String, ext: String) = File(cdn.fixture, "v1/${id.take(6)}/$id.$ext").readBytes()

    /** The file on the device's card (map_store's layout: /maps/tiles/<first 6>/<rest>.<ext>). */
    private fun cardFile(id: String, ext: String) = File(sim.dir, "sd/maps/tiles/${id.take(6)}/${id.drop(6)}.$ext")

    private fun tileGets(paths: List<String>) = paths.filter { it.endsWith(".ebm") }
    private fun poiGets(paths: List<String>) = paths.filter { it.endsWith(".poi") }
    private fun overpass(paths: List<String>) = paths.filter { it.endsWith("/api/interpreter") }

    /** Tiles and POI files the device saved since [seq] (its "tile saved:" log). */
    private fun savedSince(seq: Long): List<String> =
        sim.logSince(seq).mapNotNull { Regex("tile saved: (\\S+)").find(it)?.groupValues?.get(1) }

    /** The device holds exactly the fixture's tiles, each with its CDN version and bytes. */
    private fun assertDeviceHoldsTheArea(except: Map<String, Pair<Int, String>> = emptyMap()) {
        val onDevice = sim.tiles()
        assertEquals("the device's tiles are exactly the area's", ids, onDevice.keys)
        for (c in cdn.cells) {
            val id = c.tile.id
            val (size, hash) = except[id] ?: (c.ebmSize to c.ebmHash)
            assertEquals("device version record of $id", "cdn:$hash", onDevice[id])
            val f = cardFile(id, "ebm")
            assertTrue("$id on the card", f.isFile)
            assertEquals("$id size on the card", size, f.length().toInt())
            if (id !in except) assertArrayEquals("$id bytes on the card", fixtureBytes(id, "ebm"), f.readBytes())
        }
        val pois = sim.tiles(poi = true)
        assertEquals("the device has a .poi for every hex", ids, pois.keys)
        for (c in cdn.cells) {
            assertEquals("device .poi record of ${c.tile.id}", "cdn:${c.poiHash}", pois[c.tile.id])
            if (c.poiSize > 0) {
                assertArrayEquals("${c.tile.id}.poi bytes on the card", fixtureBytes(c.tile.id, "poi"),
                    cardFile(c.tile.id, "poi").readBytes())
            }
        }
    }

    private fun assertNoInternet(paths: List<String>) {
        // Every request the run made reached the stand-in; it has no Overpass
        // data, so a selection inside the fixture must never ask for it.
        assertEquals("Overpass was asked: $paths", emptyList<String>(), overpass(paths))
    }

    // ---- scenarios ---------------------------------------------------------------------------

    /**
     * Download an area: every tile comes from the CDN (no Overpass), lands in
     * the app's cache with its CDN version, and reaches the device, which then
     * holds exactly those tiles and .poi files with the CDN's version records
     * and bytes — and draws them where the rider is.
     */
    @Test
    fun downloadedAreaReachesTheDeviceWithItsVersions() {
        val ble = connect()
        assertTrue("a factory-fresh device has no tiles", sim.tiles().isEmpty())
        val (lat, lon) = cdn.center
        sim.cmd("gps", "lat" to lat, "lon" to lon, "fix" to true, "sats" to 9)
        val before = screenshotOfMap("before")

        val status = download(ble)
        assertNull("the Maps screen's closing status", status)

        // The app: its cache holds each tile and .poi with the CDN's version.
        runBlocking {
            for (c in cdn.cells) {
                val t = TileCache.reusable(c.tile.id, c.ebmHash, allowAged = false)
                assertNotNull("${c.tile.id} in the app's tile cache", t)
                assertEquals("cdn:${c.ebmHash}", t!!.second)
                assertArrayEquals(fixtureBytes(c.tile.id, "ebm"), t.first)
                val p = PoiCache.reusable(c.tile.id, c.poiHash, allowAged = false)
                assertNotNull("${c.tile.id}.poi in the app's POI cache", p)
                assertEquals("cdn:${c.poiHash}", p!!.second)
            }
        }
        // The app's view of the device agrees.
        assertEquals(ids, ble.deviceTileIds)
        for (c in cdn.cells) assertEquals("cdn:${c.ebmHash}", ble.tileRecords[c.tile.id])

        // The device.
        assertDeviceHoldsTheArea()

        // Each file fetched once, all from the stand-in.
        val paths = cdn.paths()
        assertEquals("each tile fetched once", ids.map { "/v1/${it.take(6)}/$it.ebm" }.sorted(), tileGets(paths).sorted())
        assertEquals("each .poi with POIs fetched once",
            cdn.cells.filter { it.poiSize > 0 }.map { "/v1/${it.tile.id.take(6)}/${it.tile.id}.poi" }, poiGets(paths))
        assertNoInternet(paths)

        // And it draws them: the map screen at the rider's position is no
        // longer "NO MAP HERE".
        val after = screenshotOfMap("after")
        assertTrue("the device draws no map where it had none ($before dark pixels)", before < 200)
        assertTrue("the device draws the downloaded map ($after dark pixels, $before before)", after > 2_000)
    }

    /**
     * The CDN publishes a new version of one hex: Redownload fetches and sends
     * that hex only, and the device records its new version; the others are
     * untouched.
     */
    @Test
    fun newCdnVersionResendsOnlyThatHex() {
        val ble = connect()
        assertNull(download(ble))
        assertDeviceHoldsTheArea()

        val changed = cdn.cells.first().tile.id
        cdn.reset()
        val v2 = cdn.bump(changed)
        val v2size = v2.get("size").asInt
        val v2hash = v2.get("hash").asString
        assertEquals("the index has the new version", v2hash, cdn.index(changed)[1].asString)
        // The app keeps an index for an hour (index.json's max-age): an hour later.
        PrebuiltTiles.IndexCache.clear()
        val seq = sim.logSeq()

        val status = download(ble, redownload = true)
        assertNull("the Maps screen's closing status", status)

        val paths = cdn.paths()
        assertEquals("only the changed tile fetched", listOf("/v1/${changed.take(6)}/$changed.ebm"), tileGets(paths))
        assertEquals("no .poi fetched (none changed)", emptyList<String>(), poiGets(paths))
        assertNoInternet(paths)
        val saved = savedSince(seq)
        assertEquals("the device saved only the changed tile: $saved", 1, saved.size)
        assertTrue(saved.single().endsWith("${changed.drop(6)}.ebm"))
        assertDeviceHoldsTheArea(except = mapOf(changed to (v2size to v2hash)))
        assertArrayEquals("the new version's bytes on the card", cdn.get("v1/${changed.take(6)}/$changed.ebm"),
            cardFile(changed, "ebm").readBytes())
        assertEquals("cdn:$v2hash", ble.tileRecords[changed])
        runBlocking {
            assertEquals("the app's cache has the new version", "cdn:$v2hash",
                TileCache.reusable(changed, v2hash, allowAged = false)?.second)
        }
    }

    /**
     * Nothing changed on the CDN: Redownload says the area is up to date and
     * sends nothing (no tile fetched, nothing saved on the device).
     */
    @Test
    fun redownloadOfCurrentAreaSendsNothing() {
        val ble = connect()
        assertNull(download(ble))
        PrebuiltTiles.IndexCache.clear()   // a fresh look at the index
        cdn.reset()
        val seq = sim.logSeq()

        val status = download(ble, redownload = true)
        assertEquals("All ${ids.size} hexes are up to date.", status)
        val paths = cdn.paths()
        assertEquals("only the index was read: $paths", listOf("/v1/862a33/index.json"), paths)
        assertEquals("the device saved nothing", emptyList<String>(), savedSince(seq))
        assertFalse(ble.tilesUploading)
        assertDeviceHoldsTheArea()
    }

    /**
     * The CDN misbehaves: one tile arrives truncated (a 200 with half the
     * body), another is a 500. The app must not send either to the device; it
     * falls back to Overpass for them (the stand-in: 503, as a busy server),
     * says the download failed, and sends the rest.
     */
    @Test
    fun badTilesFromTheCdnAreReportedAndNotSent() {
        val ble = connect()
        val truncated = cdn.cells[1].tile.id
        val failing = cdn.cells[2].tile.id
        cdn.fault("*/$truncated.ebm", "truncate")
        cdn.fault("*/$failing.ebm", "status:500")

        val status = download(ble)
        assertNotNull("the app reports the failure", status)
        assertTrue("the status names the failed fetch: $status", status!!.contains("busy") || status.contains("failed"))

        val onDevice = sim.tiles()
        assertFalse("the truncated tile reached the device", truncated in onDevice)
        assertFalse("the failed tile reached the device", failing in onDevice)
        assertFalse(cardFile(truncated, "ebm").exists())
        assertEquals("the rest reached the device", ids - truncated - failing, onDevice.keys)
        for (c in cdn.cells) onDevice[c.tile.id]?.let { assertEquals("cdn:${c.ebmHash}", it) }
        runBlocking {
            assertNull("the truncated tile is not cached", TileCache.reusable(truncated, null, allowAged = true))
            assertNull("the failed tile is not cached", TileCache.reusable(failing, null, allowAged = true))
        }
        assertFalse(ble.deviceTileIds.contains(truncated))
        val paths = cdn.paths()
        assertTrue("the fallback went to the stand-in's Overpass: $paths", overpass(paths).isNotEmpty())
    }

    // ---- the device's panel ------------------------------------------------------------------

    /**
     * Dark pixels on the device's map screen, away from the fixed furniture
     * (status bar, compass, zoom buttons, the "NO MAP HERE" box, the position
     * marker, the scale and the data fields): roads, water and parks.
     */
    private fun screenshotOfMap(name: String): Int {
        sim.cmd("console", "line" to "screen map", "wait_ms" to 500)
        waitFor("the map screen", 10_000) { sim.status().get("screen").asString == "map" }
        runFor(2_500)   // let it draw the ground around the fix
        val out = File(System.getProperty("java.io.tmpdir"), "otp-maps-${System.nanoTime()}-$name.png")
        sim.cmd("screenshot", "path" to out.path)
        val (w, px) = grayPng(out.readBytes())
        var dark = 0
        for (y in 70 until 745) for (x in 0 until 455) {          // above the scale bar
            if (y in 395..505) continue                       // the "NO MAP HERE" box and the marker
            if (y < 150 && x > 440) continue
            if ((px[y * w + x].toInt() and 0xFF) < 100) dark++
        }
        println("screenshot $name: $out, $dark dark pixels")
        return dark
    }

    /** The sim's screenshots: 8-bit grayscale PNG, filter 0 on every row
     *  (sim/host/epd_sim.cpp). Returns the width and the pixels. */
    private fun grayPng(b: ByteArray): Pair<Int, ByteArray> {
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
