package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.data.DashLayout
import com.raemond.opentrailpaper.data.DashSize
import com.raemond.opentrailpaper.data.FitDecoder
import com.raemond.opentrailpaper.data.GpxExporter
import com.raemond.opentrailpaper.data.LatLon
import com.raemond.opentrailpaper.map.MapBuilder
import com.raemond.opentrailpaper.map.MapTile
import com.raemond.opentrailpaper.map.OsmData
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.util.Locale

/**
 * The formats this app shares with the firmware, the iOS companion and the
 * Python/JS map builders.
 *
 * These are the ports where a mistake is silent: a wrong byte order or a
 * locale-formatted decimal produces a file that transfers cleanly and then draws
 * the wrong thing on the device — or nothing. Everything here runs on the JVM
 * with no device and no network.
 */
class FormatTest {

    // MARK: .ebm tiles

    /** Two crossing residential ways over a small box, as Overpass would return them. */
    private val sampleOsm = """
        {"elements":[
          {"type":"node","id":1,"lat":37.7700,"lon":-122.4200},
          {"type":"node","id":2,"lat":37.7750,"lon":-122.4200},
          {"type":"node","id":3,"lat":37.7700,"lon":-122.4150},
          {"type":"node","id":4,"lat":37.7750,"lon":-122.4150},
          {"type":"way","id":10,"nodes":[1,2],"tags":{"highway":"motorway"}},
          {"type":"way","id":11,"nodes":[1,3],"tags":{"highway":"residential"}},
          {"type":"way","id":12,"nodes":[1,2,4,3,1],"tags":{"natural":"water"}},
          {"type":"way","id":13,"nodes":[1,3,4,2,1],"tags":{"leisure":"park"}},
          {"type":"way","id":14,"nodes":[1,3],"tags":{"highway":"footway","footway":"sidewalk"}}
        ]}
    """.trimIndent()

    private val tile = MapTile(
        id = "86283082fffffff",
        cell = 0L,
        south = 37.769,
        west = -122.421,
        north = 37.776,
        east = -122.414,
    )

    private fun parse() = OsmData.parse(sampleOsm.byteInputStream())

    @Test
    fun `ebm header matches the format the firmware reads`() {
        val blob = MapBuilder.encodeTiles(parse(), listOf(tile)).single().second

        assertEquals("EBM2", String(blob, 0, 4, Charsets.US_ASCII))
        // lat0/lon0 are the tile's south-west snapped DOWN to the 0.02 deg grid.
        assertEquals(37.76, f64(blob, 4), 1e-9)
        assertEquals(-122.44, f64(blob, 12), 1e-9)
        assertEquals(MapBuilder.TILE_DEG, f64(blob, 20), 1e-9)
        val nx = i32(blob, 28)
        val ny = i32(blob, 32)
        assertEquals(2, nx)          // ceil((-122.414 + 122.44) / 0.02)
        assertEquals(1, ny)          // ceil((37.776 - 37.76) / 0.02)
        assertEquals(36 + nx * ny * 8, indexEnd(nx, ny))
        assertTrue("tile should carry road data", blob.size > 36 + nx * ny * 8)
    }

    @Test
    fun `road classes are numbered as the firmware enum expects`() {
        // motorway -> 0 (arterial), residential -> 4 (minor); a sidewalk footway
        // is dropped, which is what stops every pavement doubling every street.
        assertEquals(0, OsmData.classify("motorway", null))
        assertEquals(0, OsmData.classify("motorway_link", null))
        assertEquals(1, OsmData.classify("primary", null))
        assertEquals(2, OsmData.classify("secondary", null))
        assertEquals(3, OsmData.classify("tertiary", null))
        assertEquals(4, OsmData.classify("residential", null))
        assertEquals(5, OsmData.classify("cycleway", null))
        assertEquals(-1, OsmData.classify("footway", "sidewalk"))
        assertEquals(-1, OsmData.classify("footway", "crossing"))
        assertEquals(5, OsmData.classify("footway", null))
        assertEquals(-1, OsmData.classify(null, null))
        assertEquals(-1, OsmData.classify("raceway", null))
    }

    @Test
    fun `water and park sections append with their own magic`() {
        val osm = parse()
        val out = ByteArrayOutputStream()
        out.write(MapBuilder.encodeTiles(osm, listOf(tile)).single().second)
        val afterRoads = out.size()

        MapBuilder.appendWater(
            out, MapBuilder.waterWays(osm), emptyList(),
            tile.south, tile.west, tile.north, tile.east,
        )
        MapBuilder.appendParks(
            out, MapBuilder.parkWays(osm),
            tile.south, tile.west, tile.north, tile.east,
        )
        val blob = out.toByteArray()

        assertEquals("WTR2", String(blob, afterRoads, 4, Charsets.US_ASCII))
        val waterPolys = u16(blob, afterRoads + 4)
        assertEquals(1, waterPolys)
        // <u16 count><u16 pointCount><i16 x,y ...>
        val waterPoints = u16(blob, afterRoads + 6)
        val afterWater = afterRoads + 8 + waterPoints * 4
        assertEquals("PRK2", String(blob, afterWater, 4, Charsets.US_ASCII))
        assertEquals(1, u16(blob, afterWater + 4))
        assertTrue("a ring needs at least three points", waterPoints >= 3)
    }

    @Test
    fun `an elevation block is exactly the size the device indexes by`() {
        val out = ByteArrayOutputStream()
        val n = 4
        MapBuilder.appendElevation(
            out, tile.south, tile.west, tile.north, tile.east,
            ShortArray(n * n) { it.toShort() }, n,
        )
        val blob = out.toByteArray()
        assertEquals("ELV1", String(blob, 0, 4, Charsets.US_ASCII))
        assertEquals(n, i32(blob, 4))
        assertEquals(n, i32(blob, 8))
        // magic + 2 i32 + 4 f64 + n*n i16
        assertEquals(44 + n * n * 2, blob.size)
    }

    @Test
    fun `a tile with nothing in it is recognised as empty`() {
        val empty = MapBuilder.encodeTiles(
            OsmData.parse("""{"elements":[]}""".byteInputStream()),
            listOf(tile),
        ).single().second
        assertTrue(MapBuilder.isEmpty(empty, tile))

        val withRoads = MapBuilder.encodeTiles(parse(), listOf(tile)).single().second
        assertTrue(!MapBuilder.isEmpty(withRoads, tile))
    }

    // MARK: GPX

    @Test
    fun `gpx coordinates use a dot whatever the phone's locale`() {
        val original = Locale.getDefault()
        try {
            // Germany formats decimals with a comma; the device's parser scans
            // for a number and would read lat="37,7749" as 37.
            Locale.setDefault(Locale.GERMANY)
            val gpx = GpxExporter.make("ride", listOf(LatLon(37.7749, -122.4194)))
            assertTrue(gpx, gpx.contains("lat=\"37.774900\""))
            assertTrue(gpx, gpx.contains("lon=\"-122.419400\""))
            assertTrue(gpx, !gpx.contains(","))
        } finally {
            Locale.setDefault(original)
        }
    }

    @Test
    fun `gpx escapes a name that would otherwise break the xml`() {
        val gpx = GpxExporter.make("Ben & <Jerry>", emptyList())
        assertTrue(gpx, gpx.contains("<name>Ben &amp; &lt;Jerry&gt;</name>"))
    }

    // MARK: dashboard layout

    @Test
    fun `dashboard config survives a round trip through the device's text format`() {
        val layout = DashLayout.deviceDefault
        val reparsed = DashLayout.parse(layout.configText)
        assertEquals(layout, reparsed)
        assertEquals(layout.configText, reparsed.configText)

        // The packer's surprise, duplicated from ui_render.cpp: a lone `half`
        // spans the row rather than pairing with a full-width neighbour.
        assertEquals(3, layout.rows.size)
        assertEquals(1, layout.rows[0].size)
        assertEquals(2, layout.rows[1].size)
    }

    @Test
    fun `a hand-edited config with a typo still opens`() {
        val parsed = DashLayout.parse(
            """
            # comment
            speed      large
            nonsense   medium
            hr         medium half
            cadence    bogus  half
            """.trimIndent(),
        )
        assertEquals(listOf("speed", "hr", "cadence"), parsed.items.map { it.field })
        // An unknown size falls back to medium rather than dropping the field.
        assertEquals(DashSize.MEDIUM, parsed.items[2].size)
        assertEquals(DashSize.LARGE, parsed.items[0].size)
    }

    @Test
    fun `the parser stops at the device's item limit`() {
        val text = (1..20).joinToString("\n") { "speed medium" }
        assertEquals(DashLayout.MAX_ITEMS, DashLayout.parse(text).items.size)
    }

    // MARK: FIT

    @Test
    fun `a ride recorded by the device decodes`() {
        val bytes = javaClass.classLoader!!.getResourceAsStream("demo.fit")!!.readBytes()
        val ride = FitDecoder.decode(bytes)
        assertNotNull("demo.fit should decode", ride)
        ride!!
        assertTrue("expected a track", ride.points.size > 10)
        assertTrue("distance should be positive", ride.distanceKm > 0)
        assertTrue("duration should be positive", ride.duration > 0)
        // Semicircles -> degrees; anything else lands in the wrong ocean.
        val first = ride.points.first().coordinate
        assertTrue("lat $first out of range", first.lat > -90 && first.lat < 90)
        assertTrue("lon $first out of range", first.lon > -180 && first.lon < 180)
    }

    @Test
    fun `a truncated ride is rejected rather than half-read`() {
        assertNull(FitDecoder.decode(ByteArray(8)))
    }

    private fun assertNull(v: Any?) = assertTrue("expected null, got $v", v == null)

    // MARK: little-endian readers, mirroring the firmware's

    private fun indexEnd(nx: Int, ny: Int) = 36 + nx * ny * 8

    private fun u16(b: ByteArray, i: Int) =
        (b[i].toInt() and 0xFF) or ((b[i + 1].toInt() and 0xFF) shl 8)

    private fun i32(b: ByteArray, i: Int): Int {
        var v = 0
        for (k in 3 downTo 0) v = (v shl 8) or (b[i + k].toInt() and 0xFF)
        return v
    }

    private fun f64(b: ByteArray, i: Int): Double {
        var bits = 0L
        for (k in 7 downTo 0) bits = (bits shl 8) or (b[i + k].toLong() and 0xFF)
        return Double.fromBits(bits)
    }
}
