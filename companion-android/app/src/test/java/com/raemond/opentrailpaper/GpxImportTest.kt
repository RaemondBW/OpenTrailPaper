package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.data.GpxImporter
import com.raemond.opentrailpaper.data.ImportResult
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

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
}
