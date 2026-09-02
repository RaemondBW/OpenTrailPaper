package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.ble.Maneuver
import com.raemond.opentrailpaper.data.DeviceText
import com.raemond.opentrailpaper.data.GpxExporter
import com.raemond.opentrailpaper.data.GpxImporter
import com.raemond.opentrailpaper.data.ImportResult
import com.raemond.opentrailpaper.data.LatLon
import com.raemond.opentrailpaper.routing.Routing
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.Locale

/**
 * Reading someone else's GPX.
 *
 * The shapes here are copied from real Komoot and Strava exports, because the
 * two mistakes that matter are both shape mistakes: picking up the author's
 * name instead of the route's, and treating a waypoint as a route point.
 */
class GpxImportTest {

    private fun parse(xml: String, fallback: String = "fallback") =
        GpxImporter.parse(xml.byteInputStream(), fallback)

    private fun ok(xml: String, fallback: String = "fallback") =
        (parse(xml, fallback) as ImportResult.Ok).route

    /** A Komoot export: metadata name, author link, trk name, trk type. */
    private val komoot = """
        <?xml version='1.0' encoding='UTF-8'?>
        <gpx version="1.1" creator="https://www.komoot.de"
             xmlns="http://www.topografix.com/GPX/1/1">
          <metadata>
            <name>Metadata Name</name>
            <author><link href="https://www.komoot.de">
              <text>komoot</text><type>text/html</type>
            </link></author>
          </metadata>
          <trk>
            <name>Track Name</name>
            <type>racebike</type>
            <trkseg>
              <trkpt lat="49.337633" lon="20.005195"><ele>734.6</ele></trkpt>
              <trkpt lat="49.337637" lon="20.005103"><ele>734.6</ele></trkpt>
            </trkseg>
            <trkseg>
              <trkpt lat="49.337728" lon="20.004207"><ele>734.6</ele></trkpt>
            </trkseg>
          </trk>
        </gpx>
    """.trimIndent()

    @Test
    fun `reads track points across every segment`() {
        val r = ok(komoot)
        assertEquals(3, r.points.size)
        assertEquals(49.337633, r.points[0].lat, 1e-9)
        assertEquals(20.005195, r.points[0].lon, 1e-9)
        assertEquals(20.004207, r.points[2].lon, 1e-9)
    }

    /**
     * Komoot nests <type>text/html</type> inside <author><link>. A parser that
     * takes the first <type> it sees reports the author's link type as the
     * activity — this actually happened while measuring the sample files.
     */
    @Test
    fun `takes name and type from the track, not the author block`() {
        val r = ok(komoot)
        assertEquals("Track Name", r.name)
        assertEquals("racebike", r.activityType)
        assertEquals("Cycling", r.mode)
    }

    /** Strava puts the athlete's name in <metadata><author><name>. */
    @Test
    fun `ignores the author name in a strava export`() {
        val strava = """
            <?xml version="1.0" encoding="UTF-8"?>
            <gpx creator="StravaGPX" version="1.1"
                 xmlns="http://www.topografix.com/GPX/1/1">
             <metadata>
              <author><name>Some Athlete</name></author>
             </metadata>
             <trk>
              <name>WR objazd 2026</name>
              <type>cycling</type>
              <trkseg>
               <trkpt lat="52.240190" lon="20.889220"><ele>107.07</ele></trkpt>
               <trkpt lat="52.240280" lon="20.887850"><ele>106.98</ele></trkpt>
              </trkseg>
             </trk>
            </gpx>
        """.trimIndent()
        assertEquals("WR objazd 2026", ok(strava).name)
    }

    /**
     * The firmware's scanner is tag-agnostic: it strstr's for lat=" then lon="
     * and pairs them whatever tag they sit in (routes.cpp:319). A file with
     * leading waypoints would have them dragged into the polyline in file
     * order. This is the reason we parse and re-emit rather than pass through.
     */
    @Test
    fun `ignores waypoints entirely`() {
        val withWpt = """
            <gpx version="1.1" xmlns="http://www.topografix.com/GPX/1/1">
              <wpt lat="0.0" lon="0.0"><name>Parking</name></wpt>
              <wpt lat="1.0" lon="1.0"><name>Cafe</name></wpt>
              <trk><name>Real Route</name><trkseg>
                <trkpt lat="52.1" lon="21.1"></trkpt>
                <trkpt lat="52.2" lon="21.2"></trkpt>
              </trkseg></trk>
            </gpx>
        """.trimIndent()
        val r = ok(withWpt)
        assertEquals(2, r.points.size)
        assertEquals(52.1, r.points[0].lat, 1e-9)
    }

    @Test
    fun `falls back to route points when there is no track`() {
        val rte = """
            <gpx version="1.1" xmlns="http://www.topografix.com/GPX/1/1">
              <rte><name>Garmin Style</name>
                <rtept lat="52.1" lon="21.1"></rtept>
                <rtept lat="52.2" lon="21.2"></rtept>
              </rte>
            </gpx>
        """.trimIndent()
        val r = ok(rte)
        assertEquals(2, r.points.size)
        assertEquals("Garmin Style", r.name)
    }

    @Test
    fun `falls back to metadata name then to the filename`() {
        val metaOnly = """
            <gpx version="1.1" xmlns="http://www.topografix.com/GPX/1/1">
              <metadata><name>From Metadata</name></metadata>
              <trk><trkseg>
                <trkpt lat="52.1" lon="21.1"></trkpt>
                <trkpt lat="52.2" lon="21.2"></trkpt>
              </trkseg></trk>
            </gpx>
        """.trimIndent()
        assertEquals("From Metadata", ok(metaOnly).name)

        val noNames = """
            <gpx version="1.1" xmlns="http://www.topografix.com/GPX/1/1">
              <trk><trkseg>
                <trkpt lat="52.1" lon="21.1"></trkpt>
                <trkpt lat="52.2" lon="21.2"></trkpt>
              </trkseg></trk>
            </gpx>
        """.trimIndent()
        assertEquals("my ride", ok(noNames, "my ride").name)
    }

    @Test
    fun `rejects something that is not gpx`() {
        assertTrue(parse("<html><body>nope</body></html>") is ImportResult.NotGpx)
        assertTrue(parse("this is not xml at all {}") is ImportResult.NotGpx)
    }

    @Test
    fun `rejects a gpx with fewer than two points`() {
        val one = """
            <gpx version="1.1" xmlns="http://www.topografix.com/GPX/1/1">
              <trk><trkseg><trkpt lat="52.1" lon="21.1"></trkpt></trkseg></trk>
            </gpx>
        """.trimIndent()
        assertTrue(parse(one) is ImportResult.NoPoints)

        val none = """
            <gpx version="1.1" xmlns="http://www.topografix.com/GPX/1/1">
              <metadata><name>Empty</name></metadata>
            </gpx>
        """.trimIndent()
        assertTrue(parse(none) is ImportResult.NoPoints)
    }

    @Test
    fun `decimates a file past the firmware point cap`() {
        val sb = StringBuilder("<gpx version=\"1.1\" xmlns=\"http://www.topografix.com/GPX/1/1\"><trk><trkseg>")
        for (i in 0 until 10_000) {
            sb.append("<trkpt lat=\"52.").append(1000000 + i).append("\" lon=\"21.0\"></trkpt>")
        }
        sb.append("</trkseg></trk></gpx>")
        val r = ok(sb.toString())
        assertTrue("got ${r.points.size}", r.points.size <= GpxImporter.MAX_POINTS)
        assertTrue(r.points.size > GpxImporter.MAX_POINTS / 2)
    }

    @Test
    fun `distance comes out of the geometry`() {
        val r = ok(komoot)
        // Three points a few tens of metres apart in the Tatra foothills.
        assertTrue("got ${r.distanceMeters}", r.distanceMeters in 50.0..250.0)
    }

    @Test
    fun `mode is blank when the file does not say what it is for`() {
        val noType = """
            <gpx version="1.1" xmlns="http://www.topografix.com/GPX/1/1">
              <trk><trkseg>
                <trkpt lat="52.1" lon="21.1"></trkpt>
                <trkpt lat="52.2" lon="21.2"></trkpt>
              </trkseg></trk>
            </gpx>
        """.trimIndent()
        assertEquals("", ok(noType).mode)
        assertNull(ok(noType).activityType)
    }

    @Test
    fun `an external entity gets nothing, whatever it names`() {
        val xxe = """
            <?xml version="1.0"?>
            <!DOCTYPE gpx [<!ENTITY leak SYSTEM "file:///etc/passwd">]>
            <gpx version="1.1" xmlns="http://www.topografix.com/GPX/1/1">
              <trk><trkseg>
                <trkpt lat="52.1" lon="21.1"></trkpt>
                <trkpt lat="52.2" lon="21.2"></trkpt>
              </trkseg><name>&leak;</name></trk>
            </gpx>
        """.trimIndent()
        // A .gpx arrives from any app on the phone, so a file that names a path
        // must come back with the entity empty rather than the file in it.
        // On the JVM the factory feature alone already refuses it — the parser
        // here is Xerces, which is exactly why this cannot stand in for the
        // device. It guards the property, not the mechanism.
        assertEquals("fallback", ok(xxe).name)
    }

    @Test
    fun `a german phone still round-trips a route through a dot`() {
        val original = Locale.getDefault()
        try {
            // Both halves have to hold under a comma-decimal locale: the parse
            // reads a dotted attribute as a number, and the export writes it
            // back dotted for the device's scanner.
            Locale.setDefault(Locale.GERMANY)
            val r = ok(komoot)
            val gpx = GpxExporter.make("ride", r.points)
            assertTrue(gpx, gpx.contains("lat=\"49.337633\""))
            assertTrue(gpx, gpx.contains("lon=\"20.005195\""))
            assertTrue(gpx, !gpx.contains(","))
        } finally {
            Locale.setDefault(original)
        }
    }

    // MARK: device text budgets

    @Test
    fun `route filename folds accents away and stays inside the name budget`() {
        val n = DeviceText.routeFileName("From Poronin to Kościelisko")
        assertEquals("from_poronin_to_koscielisko.gpx", n)
        assertTrue(n, n.toByteArray(Charsets.UTF_8).size <= 31)
        assertTrue(n, n.all { it.code < 128 })
    }

    @Test
    fun `route filename truncates by bytes and never ends in an underscore`() {
        val n = DeviceText.routeFileName("Petla z Leszna na polnoc i jeszcze dalej na wschod")
        assertTrue(n, n.toByteArray(Charsets.UTF_8).size <= 31)
        assertTrue(n, n.endsWith(".gpx"))
        assertTrue(n, !n.removeSuffix(".gpx").endsWith("_"))
    }

    @Test
    fun `route filename falls back when the title has nothing usable in it`() {
        assertEquals("route.gpx", DeviceText.routeFileName(null))
        assertEquals("route.gpx", DeviceText.routeFileName("   "))
        assertEquals("route.gpx", DeviceText.routeFileName("!!! ???"))
    }

    /**
     * The firmware copies cue text into a char[48] with snprintf, which cuts at
     * 47 bytes with no regard for UTF-8. "Ł" is two bytes; splitting it puts an
     * invalid sequence on the panel. Already reachable from search-built routes
     * down any long Polish street — imports only make it common.
     */
    @Test
    fun `maneuver text truncates on a codepoint boundary`() {
        val long = "Turn slightly right onto Jakuba Ignacego Łaszczyńskiego"
        assertTrue(long.toByteArray(Charsets.UTF_8).size > DeviceText.MANEUVER_BUDGET)

        val fitted = DeviceText.maneuverText(long)
        val bytes = fitted.toByteArray(Charsets.UTF_8)
        assertTrue("${bytes.size} bytes", bytes.size <= DeviceText.MANEUVER_BUDGET)
        // Round-trips cleanly: no half-character at the end.
        assertEquals(fitted, String(bytes, Charsets.UTF_8))
        assertTrue(fitted, fitted.startsWith("Turn slightly right"))
    }

    @Test
    fun `maneuver text leaves a cue that already fits completely alone`() {
        val short = "Turn left onto Ustup"
        assertEquals(short, DeviceText.maneuverText(short))
    }

    @Test
    fun `maneuver text prefers to cut at a word boundary`() {
        val fitted = DeviceText.maneuverText("Turn right onto Aleja Krakowska Wschodnia Poludniowa")
        assertTrue(fitted, !fitted.endsWith(" "))
        assertTrue(fitted, fitted.toByteArray(Charsets.UTF_8).size <= DeviceText.MANEUVER_BUDGET)
    }

    @Test
    fun `fitUtf8 never splits a multi-byte character`() {
        // 20 two-byte characters against a 15-byte budget.
        val accented = "ą".repeat(20)
        val fitted = DeviceText.fitUtf8(accented, 15)
        val bytes = fitted.toByteArray(Charsets.UTF_8)
        assertTrue("${bytes.size}", bytes.size <= 15)
        assertEquals(fitted, String(bytes, Charsets.UTF_8))
        assertEquals(7, fitted.length)
    }

    // MARK: cue derivation (the pure parts — the OSRM call itself is network)

    private fun line(n: Int) = (0 until n).map { LatLon(52.0 + it * 0.001, 21.0) }

    @Test
    fun `via sampling keeps the ends and returns the count asked for`() {
        val s = Routing.sampleByDistance(line(1000), 50)
        assertEquals(50, s.size)
        assertEquals(52.0, s.first().lat, 1e-9)
        assertEquals(line(1000).last().lat, s.last().lat, 1e-9)
    }

    @Test
    fun `via sampling spaces points by distance, not by index`() {
        // Dense at the start, sparse after: sampling by index would put almost
        // every via in the first kilometre.
        val dense = (0 until 500).map { LatLon(52.0 + it * 0.00001, 21.0) }
        val sparse = (1..100).map { LatLon(52.005 + it * 0.001, 21.0) }
        val s = Routing.sampleByDistance(dense + sparse, 10)
        assertEquals(10, s.size)
        val firstHalf = s.count { it.lat < 52.005 }
        assertTrue("$firstHalf of 10 vias landed in the dense head", firstHalf <= 3)
    }

    @Test
    fun `via sampling copes with a track shorter than the sample count`() {
        val s = Routing.sampleByDistance(line(3), 50)
        assertTrue(s.size in 2..3)
        assertEquals(line(3).last().lat, s.last().lat, 1e-9)
    }

    private fun cue(i: Int, filler: Boolean = false, slight: Boolean = false) =
        Routing.Cue(Maneuver(52.0 + i * 0.001, 21.0, "Turn left"), filler, slight)

    @Test
    fun `triage keeps everything when the cues fit`() {
        val r = Routing.triage((0 until 90).map { cue(it) }, cap = 128)
        assertEquals(90, r.maneuvers.size)
        assertTrue(!r.truncated)
    }

    @Test
    fun `triage drops filler before real turns`() {
        val cues = (0 until 100).map { cue(it) } + (100 until 140).map { cue(it, filler = true) }
        val r = Routing.triage(cues, cap = 128)
        assertEquals(100, r.maneuvers.size)
        assertTrue(!r.truncated)
    }

    @Test
    fun `triage drops slight turns only after filler is gone`() {
        val cues = (0 until 120).map { cue(it) } +
            (120 until 140).map { cue(it, slight = true) } +
            (140 until 150).map { cue(it, filler = true) }
        val r = Routing.triage(cues, cap = 128)
        assertEquals(120, r.maneuvers.size)
        assertTrue(!r.truncated)
    }

    @Test
    fun `triage truncates and says so when even the real turns overflow`() {
        val r = Routing.triage((0 until 200).map { cue(it) }, cap = 128)
        assertEquals(128, r.maneuvers.size)
        assertTrue(r.truncated)
    }

    @Test
    fun `triage fits every cue's text to the device budget`() {
        val long = "Turn slightly right onto Jakuba Ignacego Łaszczyńskiego"
        val r = Routing.triage(listOf(Routing.Cue(Maneuver(52.0, 21.0, long), false, false)), 128)
        val bytes = r.maneuvers[0].text.toByteArray(Charsets.UTF_8)
        assertTrue("${bytes.size}", bytes.size <= DeviceText.MANEUVER_BUDGET)
    }
}
