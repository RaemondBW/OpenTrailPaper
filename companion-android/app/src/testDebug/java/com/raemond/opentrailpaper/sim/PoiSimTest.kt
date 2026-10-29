package com.raemond.opentrailpaper.sim

import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.map.CdnVersions
import com.raemond.opentrailpaper.map.PoiCache
import com.raemond.opentrailpaper.map.PrebuiltTiles
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/**
 * Cycling POIs (drinking water, toilets, repair stands, bike shops) from the
 * stand-in CDN or Overpass to the device, through the app's real download
 * (runMapDownload's POI pass: PoiCache, PrebuiltTiles.fetchPois, the Overpass
 * POI query and Cycling.buildPoi) and BleManager, against the firmware in the
 * simulator.
 *
 * The POIs are the fixture's POI sets (sim/fixtures/appmaps/pois, made by
 * sim/cdn/build_poi_fixture.py): a handful of made-up POIs in the Woodstock
 * area, with each cell's .poi built by the reference writer, docs/mapgen.js
 * buildPoi. What the device must hold is exactly those bytes, whichever way
 * the app got them: served ready-made by the CDN, synthesised for a cell the
 * CDN lists with no POIs, or built on the phone from Overpass's answer.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = SimTestApp::class)
class PoiSimTest : MapSimHarness() {

    private val poiNote = "Cycling POIs could not be fetched — try again later."

    /** The POI count of a .poi file (u16 at 6). */
    private fun count(poi: ByteArray) = (poi[6].toInt() and 0xFF) or ((poi[7].toInt() and 0xFF) shl 8)

    /** Cells of [set] with POIs in them. */
    private fun cellsWithPois(set: String) = ids.filter { count(cdn.poiSetFile(set, it)) > 0 }.toSet()

    /**
     * The device holds a .poi for every hex of the area, each the reference
     * writer's file for [set] byte for byte, with the version record [records]
     * gives it (default: the CDN's, "cdn:<hash>", "cdn:" for an empty one).
     */
    private fun assertDeviceHoldsPois(set: String, index: Map<String, Pair<Int, String>>,
                                      only: Set<String> = ids, records: Map<String, String> = emptyMap()) {
        val pois = sim.tiles(poi = true)
        assertEquals("the device's .poi files", only, pois.keys)
        for (id in only) {
            val want = cdn.poiSetFile(set, id)
            val f = cardFile(id, "poi")
            assertTrue("$id.poi on the card", f.isFile)
            assertArrayEquals("$id.poi on the card is the reference writer's (${count(want)} POIs)", want, f.readBytes())
            val record = records[id] ?: "cdn:${index.getValue(id).second}"
            if (record.endsWith("*")) {
                assertTrue("$id.poi version record ${pois[id]}", pois[id]!!.startsWith(record.dropLast(1)))
            } else {
                assertEquals("$id.poi version record", record, pois[id])
            }
        }
    }

    // ---- from the CDN ------------------------------------------------------------------------

    /**
     * Download an area with POIs: the cells with POIs get the CDN's .poi
     * files, the others the empty file the app synthesises for poiSize 0 —
     * and the device holds every one, byte for byte the reference writer's,
     * with the CDN's version records. No Overpass; each .poi fetched once.
     */
    @Test
    fun areaPoisReachTheDeviceByteForByte() {
        val index = cdn.pois("woodstock")
        val withPois = cellsWithPois("woodstock")
        assertTrue("the set has empty and non-empty cells", withPois.size in 2 until ids.size)
        val ble = connect()
        assertNull("the Maps screen's closing status", download(ble))

        assertDeviceHoldsPois("woodstock", index)
        val paths = cdn.paths()
        assertEquals("each .poi with POIs fetched once, no others",
            withPois.map { "/v1/${it.take(6)}/$it.poi" }.sorted(), poiGets(paths).sorted())
        assertNoInternet(paths)
        // The app: its POI cache and its view of the device agree.
        runBlocking {
            for (id in ids) {
                val p = PoiCache.reusable(id, index.getValue(id).second, allowAged = false)
                assertNotNull("$id.poi in the app's POI cache", p)
                assertEquals("cdn:${index.getValue(id).second}", p!!.second)
                assertArrayEquals(cdn.poiSetFile("woodstock", id), p.first)
            }
        }
        assertEquals(ids, ble.devicePoiIds)
        for (id in ids) assertFalse("$id still wants POIs", ble.poiNeedsSend(id))
    }

    /**
     * Upstream, one cell's POIs change (a new repair stand, the old one's pump
     * gone); the CDN publishes its new .poi. The Maps screen sees it from the
     * index (that hex wants POIs, no other), and its "Send POIs" download
     * fetches and sends that one file: no tile, no other .poi.
     */
    @Test
    fun poiChangeUpstreamResendsOnlyThatHex() {
        val first = cdn.pois("woodstock")
        val ble = connect()
        assertNull(download(ble))
        assertDeviceHoldsPois("woodstock", first)

        cdn.reset()
        val second = cdn.pois("changed")
        val changed = ids.filter { second[it] != first[it] }
        assertEquals("the changed set differs in one cell", 1, changed.size)
        val id = changed.single()
        // An hour later (index.json's max-age), the Maps screen looks again.
        PrebuiltTiles.IndexCache.clear()
        CdnVersions.clear()
        loadCdnVersions()
        assertEquals("the Maps screen's hexes that want POIs", listOf(id), tiles.filter { ble.poiNeedsSend(it.id) }.map { it.id })
        assertTrue("no hex is new", tiles.none { !ble.tileIsCurrent(it.id) })
        cdn.reset()
        cdn.pois("changed")
        val seq = sim.logSeq()

        assertNull("the Maps screen's closing status", download(ble))
        val paths = cdn.paths()
        assertEquals("no tile fetched", emptyList<String>(), tileGets(paths))
        assertEquals("only the changed .poi fetched", listOf("/v1/${id.take(6)}/$id.poi"), poiGets(paths))
        assertNoInternet(paths)
        val saved = savedSince(seq)
        assertEquals("the device saved only the changed .poi: $saved", 1, saved.size)
        assertTrue(saved.single().endsWith("${id.take(6)}/${id.drop(6)}.poi"))
        assertDeviceHoldsPois("changed", second)
        assertEquals("cdn:${second.getValue(id).second}", ble.poiRecords[id])
    }

    /**
     * An area with no POIs at all: the CDN lists every cell with poiSize 0,
     * so nothing is fetched; the device still gets a .poi for each hex — the
     * empty file that says "built, nothing here" — recorded "cdn:".
     */
    @Test
    fun areaWithNoPoisGetsEmptyPoiFiles() {
        val index = cdn.pois("none")
        assertTrue(index.values.all { it == (0 to "") })
        val ble = connect()
        assertNull(download(ble))
        assertDeviceHoldsPois("none", index)
        for (id in ids) assertEquals("$id.poi is header only", 40L, cardFile(id, "poi").length())
        val paths = cdn.paths()
        assertEquals("no .poi fetched", emptyList<String>(), poiGets(paths))
        assertNoInternet(paths)
        assertEquals("the device has every tile", ids, sim.tiles().keys)
    }

    /**
     * The CDN's .poi files arrive broken (a 200 with every byte flipped, the
     * index's size): none may reach the device. They fall back to Overpass
     * (the stand-in: 503), the download says the POIs could not be fetched,
     * and the map tiles, and the empty .poi of the cells without POIs, still
     * go through.
     */
    @Test
    fun garbledCdnPoisNeverReachTheDevice() {
        val index = cdn.pois("woodstock")
        cdn.fault("*.poi", "garble")
        // No Overpass POIs either: as the stand-in without a set, a busy server.
        cdn.fault("/api/interpreter", "status:503")
        assertPoisRefused(index)
    }

    // ---- from Overpass -----------------------------------------------------------------------

    /**
     * The CDN cannot serve the .poi files (500): the app asks Overpass's POI
     * query instead and builds each cell's file on the phone (H3 for the cell
     * of each POI, Cycling.buildPoi). The device must hold the reference
     * writer's bytes for the same POIs, recorded as built on this phone.
     */
    @Test
    fun overpassPoisReachTheDeviceByteForByte() {
        val h3 = HostH3.build()
        if (System.getenv("OTP_SIM_REQUIRED") != null) check(h3) { "the host H3 library (HostH3) could not be built" }
        assumeTrue("the host H3 library (HostH3) could not be built", h3)
        val index = cdn.pois("woodstock")
        val withPois = cellsWithPois("woodstock")
        cdn.fault("*.poi", "status:500")
        val ble = connect()
        assertNull("the Maps screen's closing status", download(ble))

        val queries = cdn.overpassQueries()
        assertTrue("the POIs came from Overpass's POI query: $queries", queries.isNotEmpty() && queries.all { it == "poi" })
        assertDeviceHoldsPois("woodstock", index, records = withPois.associateWith { "phone:*" })
        assertEquals("the device has every tile", ids, sim.tiles().keys)
    }

    /** Overpass answers 200 with half its JSON. */
    @Test
    fun truncatedOverpassPoisNeverReachTheDevice() = overpassFails("truncate")

    /** A proxy's HTML error page, with a 200. */
    @Test
    fun overpassErrorPageNeverReachesTheDevice() = overpassFails("garble")

    /**
     * Overpass ran out of time: a 200 with no elements and a "runtime error"
     * remark. That is not "no POIs here" and must not reach the device as
     * empty files (which it would then keep, recorded as fresh, for 30 days).
     */
    @Test
    fun overpassTimeoutNeverReachesTheDeviceAsNoPois() = overpassFails("remark")

    /** The CDN cannot serve the .poi files and Overpass answers [kind]. */
    private fun overpassFails(kind: String) {
        val index = cdn.pois("woodstock")
        cdn.fault("*.poi", "status:500")
        cdn.fault("/api/interpreter", kind)
        assertPoisRefused(index)
        assertTrue("Overpass's POI query was asked", cdn.overpassQueries().contains("poi"))
    }

    /** Downloads the area with the cells' POIs failing: those never reach the
     *  device or the app's cache; everything else does. */
    private fun assertPoisRefused(index: Map<String, Pair<Int, String>>) {
        val withPois = cellsWithPois("woodstock")
        val ble = connect()
        assertEquals("the Maps screen's closing status", poiNote, download(ble))
        assertEquals("the map tiles all reached the device", ids, sim.tiles().keys)
        for (c in cdn.cells) assertEquals("cdn:${c.ebmHash}", sim.tiles()[c.tile.id])
        // The cells the CDN lists without POIs: their empty file, as always.
        assertDeviceHoldsPois("woodstock", index, only = ids - withPois)
        for (id in withPois) assertFalse("$id.poi reached the card", cardFile(id, "poi").exists())
        runBlocking {
            for (id in withPois) assertNull("$id.poi in the app's cache", PoiCache.reusable(id, null, allowAged = true))
        }
        for (id in withPois) assertTrue("$id still wants POIs", ble.poiNeedsSend(id))
    }

    // ---- on the device's map -----------------------------------------------------------------

    /**
     * The device draws the POIs it was sent, and the rider's layer switch
     * (the Maps screen's "Device map layers": BleManager.updateMapLayers)
     * hides and shows them: with drinking water off, the water icon 150 m
     * north of the rider is gone, and nothing else changes; back on, it is
     * back.
     */
    @Test
    fun waterLayerSwitchHidesAndShowsTheIcon() {
        cdn.pois("woodstock")
        val ble = connect()
        assertNull(download(ble))
        val (lat, lon) = cdn.center
        // North up (course 0, standing), 2 m/px, the rider at (270, 430):
        // the fixture's water point, 150 m north, is drawn at about (270, 355).
        sim.cmd("gps", "lat" to lat, "lon" to lon, "fix" to true, "sats" to 9, "course" to 0, "speed_kmh" to 0)
        val all = ble.mapLayers
        assertEquals("all layers on", 0x3F, all and 0x3F)
        val on = mapScreenshot("water-on")

        ble.updateMapLayers(all and WATER.inv())
        waitFor("the device's map layers", 10_000) { layers() == (all and WATER.inv()) }
        val off = mapScreenshot("water-off")
        val (inIcon, outside) = diff(on, off)
        assertTrue("the water icon disappears ($inIcon pixels changed around it)", inIcon > 150)
        assertTrue("nothing else changes ($outside pixels changed elsewhere)", outside < 20)

        ble.updateMapLayers(all)
        waitFor("the device's map layers", 10_000) { layers() == all }
        val back = mapScreenshot("water-back")
        val (inIcon2, outside2) = diff(on, back)
        assertTrue("the water icon is back ($inIcon2 / $outside2 pixels differ)", inIcon2 < 10 && outside2 < 20)
    }

    private fun layers() = sim.status().getAsJsonObject("settings").get("map_layers")?.asInt

    /** Pixels that differ between two map screenshots near the expected water
     *  icon (270, 355) and elsewhere on the map. */
    private fun diff(a: Pair<Int, ByteArray>, b: Pair<Int, ByteArray>): Pair<Int, Int> {
        val (w, pa) = a
        val pb = b.second
        var near = 0
        var far = 0
        for (y in 70 until 745) for (x in 0 until w) {
            if (pa[y * w + x] == pb[y * w + x]) continue
            if (x in 245..295 && y in 330..380) near++ else far++
        }
        return near to far
    }

    private companion object {
        /** MAP_LAYER_WATER (src/map_view.h). */
        const val WATER = 0x04
    }
}
