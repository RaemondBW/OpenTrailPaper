package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.data.CloudRoute
import com.raemond.opentrailpaper.data.CloudRoutes
import com.raemond.opentrailpaper.data.GpxImporter
import com.raemond.opentrailpaper.data.ImportResult
import com.raemond.opentrailpaper.data.LatLon
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** The Strava / RideWithGPS route lists, and their GPX going through the file importer. */
class CloudRoutesTest {

    @Test fun `strava list keeps the string id, distance and elevation`() {
        val page = CloudRoutes.parseStrava(
            """[{"id": 3344556677889900112, "id_str": "3344556677889900112", "name": "Hills",
                 "distance": 52345.6, "elevation_gain": 812.0, "private": true},
                {"id": 17, "name": "  ", "distance": null},
                {"name": "no id"}]""",
            page = 1,
        )
        assertEquals(
            listOf(
                CloudRoute("3344556677889900112", "Hills", 52345.6, 812.0),
                CloudRoute("17", "Strava route", null, null),
            ),
            page.routes,
        )
        assertNull(page.nextPage)   // short page: nothing after it
    }

    @Test fun `a full strava page offers the next one`() {
        val full = (1..CloudRoutes.STRAVA_PAGE_SIZE).joinToString(",", "[", "]") { """{"id": $it, "name": "r$it"}""" }
        assertEquals(3, CloudRoutes.parseStrava(full, page = 2).nextPage)
    }

    @Test fun `empty or broken bodies are an empty last page`() {
        assertTrue(CloudRoutes.parseStrava("[]", 1).routes.isEmpty())
        assertNull(CloudRoutes.parseStrava("<html>", 1).nextPage)
        assertTrue(CloudRoutes.parseRwgps("{}").routes.isEmpty())
        assertNull(CloudRoutes.parseRwgps("nope").nextPage)
    }

    @Test fun `google's documented polyline decodes`() {
        val pts = CloudRoutes.decodePolyline("_p~iF~ps|U_ulLnnqC_mqNvxq`@")!!
        val want = listOf(LatLon(38.5, -120.2), LatLon(40.7, -120.95), LatLon(43.252, -126.453))
        assertEquals(want.size, pts.size)
        want.zip(pts).forEach { (w, p) ->
            assertEquals(w.lat, p.lat, 1e-9)
            assertEquals(w.lon, p.lon, 1e-9)
        }
        assertEquals(emptyList<LatLon>(), CloudRoutes.decodePolyline(""))
        assertNull(CloudRoutes.decodePolyline("_p~iF~ps|U_ulL"))   // ends after a lat
        assertNull(CloudRoutes.decodePolyline("_p~iF~ps|"))        // ends mid-number
        assertNull(CloudRoutes.decodePolyline("_p~iF ps|U"))       // not polyline characters
    }

    @Test fun `strava summary polyline becomes the route's shape`() {
        val routes = CloudRoutes.parseStrava(
            """[{"id_str": "1", "name": "A", "map": {"summary_polyline": "_p~iF~ps|U_ulLnnqC_mqNvxq`@"}},
                {"id_str": "2", "name": "B", "map": {"summary_polyline": ""}},
                {"id_str": "3", "name": "C", "map": null},
                {"id_str": "4", "name": "D", "map": {"summary_polyline": "_p~iF~ps|U"}}]""",
            page = 1,
        ).routes
        assertEquals(3, routes[0].shape?.size)
        assertEquals(38.5, routes[0].shape!!.first().lat, 1e-9)
        assertNull(routes[1].shape)
        assertNull(routes[2].shape)
        assertNull(routes[3].shape)   // one point is no outline
    }

    @Test fun `rwgps list from the sync service`() {
        val page = CloudRoutes.parseRwgps(
            """{"routes": [{"id": 11, "name": "Loop", "distance": 42000.5, "elevation_gain": 610},
                           {"id": 12, "name": null, "distance": null, "elevation_gain": null}],
                "next_page": 2}""",
        )
        assertEquals(
            listOf(CloudRoute("11", "Loop", 42000.5, 610.0), CloudRoute("12", "RideWithGPS route", null, null)),
            page.routes,
        )
        assertEquals(2, page.nextPage)
        assertNull(CloudRoutes.parseRwgps("""{"routes": [], "next_page": null}""").nextPage)
    }

    @Test fun `only read_all lists private strava routes`() {
        assertTrue(CloudRoutes.stravaSeesPrivateRoutes("read,activity:write,read_all"))
        assertFalse(CloudRoutes.stravaSeesPrivateRoutes("read,activity:write"))
        assertFalse(CloudRoutes.stravaSeesPrivateRoutes(null))   // connected before routes existed
    }

    @Test fun `strava export_gpx goes through the file importer`() {
        // Trimmed from a real export: metadata author name first, then the track.
        val gpx = """<?xml version="1.0" encoding="UTF-8"?>
            <gpx creator="StravaGPX" version="1.1" xmlns="http://www.topografix.com/GPX/1/1">
             <metadata><name>Hills</name><author><name>Ada L</name></author></metadata>
             <trk><name>Hills</name><type>cycling</type><trkseg>
              <trkpt lat="37.7" lon="-122.4"><ele>10</ele></trkpt>
              <trkpt lat="37.71" lon="-122.41"><ele>12</ele></trkpt>
             </trkseg></trk></gpx>"""
        val r = GpxImporter.parse(gpx.byteInputStream(), "fallback") as ImportResult.Ok
        assertEquals("Hills", r.route.name)
        assertEquals("Cycling", r.route.mode)
        assertEquals(2, r.route.points.size)
    }
}
