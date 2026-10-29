package com.raemond.opentrailpaper.sim

import com.raemond.opentrailpaper.map.PoiCache
import com.raemond.opentrailpaper.map.PrebuiltTiles
import com.raemond.opentrailpaper.map.TileCache
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

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
 * The harness (device, stand-in CDN, BleManager, the download) is
 * MapSimHarness; the cycling POIs have their own tests, PoiSimTest.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = SimTestApp::class)
class MapsSimTest : MapSimHarness() {

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
}
