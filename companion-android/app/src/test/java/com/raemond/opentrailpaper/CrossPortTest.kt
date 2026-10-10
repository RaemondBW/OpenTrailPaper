package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.map.Cycling
import com.raemond.opentrailpaper.map.MapBuilder
import com.raemond.opentrailpaper.map.MapTile
import com.raemond.opentrailpaper.map.OsmData
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * The cycling layer (bike-route way flags, `.poi` files) against the format the
 * firmware reads, and — driven by tools/map_test/run_crossport.sh with
 * ANDROID_CROSSPORT=1 — the Kotlin side of the cross-port byte comparison with
 * docs/mapgen.js and the iOS MapBuilder.
 */
class CrossPortTest {

    /**
     * Writes <id>.ebm (road data + way-flag trailers) and <id>.bbox.poi for each
     * tile of CROSSPORT_TILES, built from the Overpass response CROSSPORT_JSON.
     * The H3-cell .poi variant needs the JNI H3 library, which a JVM unit test
     * cannot load, so only the bbox variant is compared on this port.
     */
    @Test
    fun `cross-port build for run_crossport_sh`() {
        val json = System.getenv("CROSSPORT_JSON")
        val tiles = System.getenv("CROSSPORT_TILES")
        val out = System.getenv("CROSSPORT_OUT")
        assumeTrue("set by run_crossport.sh", json != null && tiles != null && out != null)
        val osm = File(json!!).inputStream().use { OsmData.parse(it) }
        val dir = File(out!!).apply { mkdirs() }
        for (line in File(tiles!!).readLines()) {
            val f = line.trim().split(Regex("\\s+"))
            if (f.size != 5) continue
            val t = MapTile(f[0], 0L, f[1].toDouble(), f[2].toDouble(), f[3].toDouble(), f[4].toDouble())
            val ebm = MapBuilder.encodeTiles(osm, listOf(t)).single().second
            File(dir, "${t.id}.ebm").writeBytes(ebm)
            File(dir, "${t.id}.bbox.poi").writeBytes(
                Cycling.buildPoi(osm.pois, t.south, t.west, t.north, t.east, t.id),
            )
        }
    }

    // A residential street on a regional route, a cycleway, a service road that
    // is a route member, a drinking-water node and a toilet block outline.
    private val sample = """
        {"elements":[
          {"type":"node","id":1,"lat":37.7700,"lon":-122.4200},
          {"type":"node","id":2,"lat":37.7750,"lon":-122.4200},
          {"type":"node","id":3,"lat":37.7700,"lon":-122.4150},
          {"type":"node","id":4,"lat":37.7750,"lon":-122.4150},
          {"type":"node","id":5,"lat":37.7720,"lon":-122.4180,"tags":{"amenity":"drinking_water"}},
          {"type":"way","id":10,"nodes":[1,2],"tags":{"highway":"residential","cycleway:right":"lane"}},
          {"type":"way","id":11,"nodes":[1,3],"tags":{"highway":"cycleway"}},
          {"type":"way","id":12,"nodes":[2,4],"tags":{"highway":"service"}},
          {"type":"way","id":13,"nodes":[1,2,4,3,1],"tags":{"amenity":"toilets","fee":"yes"}},
          {"type":"bikeroute","id":1,"tags":{"level":"2","ways":"10;12"}}
        ]}
    """.trimIndent()

    private val tile = MapTile("86283082fffffff", 0L, 37.769, -122.421, 37.776, -122.414)

    @Test
    fun `route members and bike flags land in the sub-tile trailer`() {
        val osm = OsmData.parse(sample.byteInputStream())
        val b = ByteBuffer.wrap(MapBuilder.encodeTiles(osm, listOf(tile)).single().second)
            .order(ByteOrder.LITTLE_ENDIAN)
        val nx = b.getInt(28)
        val ny = b.getInt(32)
        val flags = ArrayList<Int>()
        val classes = ArrayList<Int>()
        for (k in 0 until nx * ny) {
            val off = b.getInt(36 + k * 8)
            val len = b.getInt(40 + k * 8)
            if (off == 0) continue
            val count = b.getShort(off).toInt() and 0xFFFF
            var p = off + 2
            for (i in 0 until count) {
                classes.add(b.get(p).toInt() and 0xFF)
                p += 3 + (b.getShort(p + 1).toInt() and 0xFFFF) * 4
            }
            assertEquals("trailer is exactly one byte per polyline", count, off + len - p)
            for (i in 0 until count) flags.add(b.get(p + i).toInt() and 0xFF)
        }
        // residential: regional route (2) + lane (8); cycleway: 4; service road
        // pulled in as a route member: minor class 4, regional route.
        assertEquals(setOf(2 or 8, 4, 2), flags.toSet())
        assertEquals(setOf(4, 5), classes.toSet())
    }

    @Test
    fun `poi file header and records`() {
        val osm = OsmData.parse(sample.byteInputStream())
        assertEquals(2, osm.pois.size)
        val data = Cycling.buildPoi(osm.pois, tile.south, tile.west, tile.north, tile.east, tile.id)
        val b = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
        assertArrayEquals("EPOI".toByteArray(), data.copyOfRange(0, 4))
        assertEquals(1, data[4].toInt())
        assertEquals(6, data[5].toInt())
        assertEquals(2, b.getShort(6).toInt())
        assertEquals(java.lang.Long.parseUnsignedLong(tile.id, 16), b.getLong(8))
        assertEquals((tile.south + tile.north) / 2, b.getDouble(16), 0.0)
        assertEquals(40 + 2 * 6, data.size)
        val types = setOf(data[40].toInt(), data[46].toInt())
        assertEquals(setOf(Cycling.POI_WATER, Cycling.POI_TOILETS), types)
        val toiletFlags = if (data[40].toInt() == Cycling.POI_TOILETS) data[41] else data[47]
        assertEquals(0x80, toiletFlags.toInt() and 0xFF)   // fee=yes -> restricted
    }
}
