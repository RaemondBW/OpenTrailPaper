package com.raemond.opentrailpaper

import com.raemond.opentrailpaper.data.DeviceSegment
import com.raemond.opentrailpaper.data.LatLon
import com.raemond.opentrailpaper.data.StravaSegments
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.sin

/**
 * Starred segments for the device's SEGMENT page: Strava's list and detail,
 * the polyline cut, the name folding, and the blob — byte for byte the shared
 * fixture that src/segment_match.h's parser and the iOS encoder are checked
 * against (tools/segment_test).
 */
class StravaSegmentsTest {

    private val fixtureHex =
        "4f54534701000200cb04fb711f010000f2020000dd3e0000390003000b436166" +
            "c3a920636c696d6258b93900401c45ffbcb939000e1c45ff52ba3900361c45ff" +
            "2a000000000000000000000088130000fdff020004466c6174a0ad3900803b45" +
            "ff62af3900803b45ff"

    @Test
    fun `blob matches the shared fixture`() {
        val blob = StravaSegments.encode(
            listOf(
                DeviceSegment(
                    1_234_567_890_123L, "Café climb", 754, 1609.3, 5.7,
                    listOf(LatLon(37.83, -122.48), LatLon(37.831, -122.4805), LatLon(37.8325, -122.4801)),
                ),
                DeviceSegment(42, "Flat", 0, 500.0, -0.3, listOf(LatLon(37.8, -122.4), LatLon(37.8045, -122.4))),
            ),
        )
        assertEquals(fixtureHex, blob.joinToString("") { "%02x".format(it.toInt() and 0xFF) })
    }

    @Test
    fun `names are cut on a character and folded for the panel`() {
        val long = StravaSegments.encode(
            listOf(DeviceSegment(1, "é".repeat(30), 0, 1.0, 0.0, listOf(LatLon(0.0, 0.0), LatLon(0.0, 0.001)))),
        )
        assertEquals(46, long[8 + 20].toInt())   // 23 two-byte characters
        assertEquals("Cote de l'Eglise", StravaSegments.deviceName("  Côte de  l'Église "))
        assertEquals("Segment", StravaSegments.deviceName("山"))
        assertEquals(47, StravaSegments.deviceName("a".repeat(60)).length)
    }

    @Test
    fun `starred list - ids, PRs wherever they are, rides, paging`() {
        val list = """
            [{"id": 229781, "name": "Hawk Hill", "distance": 2684.82, "average_grade": 5.7,
              "activity_type": "Ride", "athlete_pr_effort": {"pr_elapsed_time": 524, "effort_count": 3}},
             {"id": 1234, "name": "Old", "distance": 100, "average_grade": 1, "activity_type": "Ride",
              "athlete_pr_effort": {"id": 9, "elapsed_time": 61}},
             {"id": 77, "name": "Run hill", "distance": 400, "average_grade": 8, "activity_type": "Run"},
             {"id": 78, "name": "Legacy", "distance": 400, "average_grade": 2, "pr_time": 99},
             {"name": "no id"}]
        """.trimIndent()
        val page = StravaSegments.parseStarred(list, 1)
        assertEquals(4, page.segments.size)
        assertNull(page.nextPage)
        assertEquals(229_781L, page.segments[0].id)
        assertEquals(524, page.segments[0].prSeconds)
        assertEquals(61, page.segments[1].prSeconds)
        assertEquals(99, page.segments[3].prSeconds)
        assertFalse(page.segments[2].isRide)
        assertTrue(page.segments[3].isRide)
        val full = "[" + List(200) { """{"id": 1, "name": "x"}""" }.joinToString(",") + "]"
        assertEquals(3, StravaSegments.parseStarred(full, 2).nextPage)
        assertTrue(StravaSegments.parseStarred("""{"message":"x"}""", 1).segments.isEmpty())

        val (poly, pr) = StravaSegments.parseDetail(
            """{"id": 77, "map": {"polyline": "_p~iF~ps|U_ulLnnqC_mqNvxq`@"}, "athlete_segment_stats": {"pr_elapsed_time": 300}}""",
        )
        assertEquals("_p~iF~ps|U_ulLnnqC_mqNvxq`@", poly)
        assertEquals(300, pr)

        val segs = StravaSegments.deviceSegments(
            page.segments,
            mapOf(229_781L to "_p~iF~ps|U_ulLnnqC_mqNvxq`@", 77L to "_p~iF~ps|U_ulLnnqC", 78L to "_p~iF~ps|U_ulLnnqC"),
            mapOf(78L to 5),
        )
        assertEquals(listOf(229_781L, 78L), segs.map { it.id })   // 1234: no polyline; 77: a run
        assertEquals(524, segs[0].prSeconds)
        assertEquals(99, segs[1].prSeconds)
        assertEquals(3, segs[0].points.size)
    }

    @Test
    fun `the cut fits the device and keeps both ends`() {
        val line = (0..100).map { LatLon(37 + it * 1e-4, -122.0) }
        assertEquals(2, StravaSegments.simplify(line).size)
        val wiggle = (0 until 2000).map { LatLon(37 + it * 1e-5, -122 + 0.01 * sin(it / 20.0)) }
        val thin = StravaSegments.simplify(wiggle)
        assertTrue(thin.size in 3..StravaSegments.MAX_POINTS)
        assertEquals(wiggle.first(), thin.first())
        assertEquals(wiggle.last(), thin.last())
    }
}
