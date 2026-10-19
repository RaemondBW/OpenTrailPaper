package com.raemond.opentrailpaper.data

import com.google.gson.JsonElement
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import java.io.ByteArrayOutputStream
import java.text.Normalizer
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min

/*
 * The rider's starred Strava segments, for the device's SEGMENT page: parsing
 * Strava's answers, cutting each polyline down to what the device keeps, and
 * the blob the device reads (src/segment_match.h). Free of android.* so it runs
 * in JVM unit tests (StravaSegmentsTest). Mirrors StravaSegments.swift; both
 * encoders are checked against the same bytes as the firmware's parser.
 */

/** One starred segment, as GET /segments/starred lists it. */
data class StarredSegment(
    val id: Long,
    val name: String,
    val distanceMeters: Double,
    val averageGrade: Double,
    /** The athlete's PR elapsed time, s; null when there is none (or the list left it out). */
    val prSeconds: Int?,
    val activityType: String?,
) {
    /** Only ride segments go to a bike computer; a list without the type is taken as rides. */
    val isRide get() = activityType == null || activityType == "Ride" || activityType == "EBikeRide"
}

data class StarredSegmentPage(val segments: List<StarredSegment>, val nextPage: Int?)

/** A segment as the device gets it. */
data class DeviceSegment(
    val id: Long,
    val name: String,
    val prSeconds: Int,
    val distanceMeters: Double,
    val averageGrade: Double,
    val points: List<LatLon>,
)

object StravaSegments {
    /** Strava's largest page. */
    const val PAGE_SIZE = 200
    /** Starred pages read at most. */
    const val MAX_PAGES = 3
    /**
     * How many go to the device. One detail request each the first time
     * (cached after), so this also keeps a first sync inside Strava's
     * 100-requests-per-15-minutes limit. The firmware takes up to 64.
     */
    const val MAX_SEGMENTS = 50
    /** Polyline points per segment; the firmware's MAX_POINTS. */
    const val MAX_POINTS = 200
    /** Name bytes; the firmware's NAME_BYTES. */
    const val NAME_BYTES = 47

    // --- Strava ---------------------------------------------------------------

    /** GET /api/v3/segments/starred?page=&per_page=: a bare array. */
    fun parseStarred(text: String, page: Int): StarredSegmentPage {
        val arr = runCatching { JsonParser.parseString(text).asJsonArray }.getOrNull()
            ?: return StarredSegmentPage(emptyList(), null)
        val segs = arr.mapNotNull { e ->
            val o = e.takeIf { it.isJsonObject }?.asJsonObject ?: return@mapNotNull null
            val id = o.prim("id")?.let { p -> runCatching { p.asLong }.getOrNull() }?.takeIf { it > 0 }
                ?: return@mapNotNull null
            StarredSegment(
                id = id,
                name = o.prim("name")?.asString?.trim().orEmpty(),
                distanceMeters = o.num("distance") ?: 0.0,
                averageGrade = o.num("average_grade") ?: 0.0,
                prSeconds = prSeconds(o),
                activityType = o.prim("activity_type")?.asString,
            )
        }
        return StarredSegmentPage(segs, if (arr.size() >= PAGE_SIZE) page + 1 else null)
    }

    /** GET /api/v3/segments/{id}: the full polyline, and the PR if the list lacked it. */
    fun parseDetail(text: String): Pair<String?, Int?> {
        val o = runCatching { JsonParser.parseString(text).asJsonObject }.getOrNull() ?: return null to null
        val poly = o.obj("map")?.prim("polyline")?.asString?.takeIf { it.isNotEmpty() }
        return poly to prSeconds(o)
    }

    /**
     * The PR, wherever this answer put it: athlete_pr_effort (the list;
     * pr_elapsed_time, or elapsed_time in older answers), athlete_segment_stats
     * (the detail), or the legacy top-level pr_time.
     */
    private fun prSeconds(o: JsonObject): Int? {
        val effort = o.obj("athlete_pr_effort")
        val stats = o.obj("athlete_segment_stats")
        for (v in listOf(effort?.num("pr_elapsed_time"), effort?.num("elapsed_time"),
            stats?.num("pr_elapsed_time"), o.num("pr_time"))) {
            if (v != null && v > 0) return roundAway(v).toInt()
        }
        return null
    }

    // --- device ---------------------------------------------------------------

    /** The name as the device's faces can draw it: accents folded away, plain ASCII, at most [NAME_BYTES]. */
    fun deviceName(s: String): String {
        val folded = Normalizer.normalize(s, Normalizer.Form.NFKD).replace(Regex("\\p{M}+"), "")
        val sb = StringBuilder()
        var lastSpace = true
        for (ch in folded) {
            val c = ch.code
            if (c < 32 || c >= 127) continue
            if (c == 32) {
                if (lastSpace) continue
                lastSpace = true
            } else {
                lastSpace = false
            }
            sb.append(ch)
        }
        var out = sb.toString().trim()
        if (out.length > NAME_BYTES) out = out.take(NAME_BYTES).trim()
        return out.ifEmpty { "Segment" }
    }

    /**
     * Douglas-Peucker in metres, the tolerance doubling from 2 m until the line
     * fits [maxPoints]. Keeps both ends; drops repeated points first.
     */
    fun simplify(pts: List<LatLon>, maxPoints: Int = MAX_POINTS): List<LatLon> {
        val p = ArrayList<LatLon>()
        for (q in pts) if (p.lastOrNull() != q) p.add(q)
        if (p.size <= 2) return p
        val kx = 111_320.0 * cos(Math.toRadians(p[0].lat))
        val ky = 110_540.0
        val xs = DoubleArray(p.size) { (p[it].lon - p[0].lon) * kx }
        val ys = DoubleArray(p.size) { (p[it].lat - p[0].lat) * ky }
        var tol = 2.0
        while (true) {
            val keep = BooleanArray(p.size)
            keep[0] = true
            keep[p.size - 1] = true
            val stack = ArrayDeque<Pair<Int, Int>>()
            stack.addLast(0 to p.size - 1)
            while (stack.isNotEmpty()) {
                val (a, b) = stack.removeLast()
                if (b <= a + 1) continue
                val dx = xs[b] - xs[a]
                val dy = ys[b] - ys[a]
                val len2 = dx * dx + dy * dy
                var worst = -1.0
                var wi = a
                for (i in a + 1 until b) {
                    var t = if (len2 > 0) ((xs[i] - xs[a]) * dx + (ys[i] - ys[a]) * dy) / len2 else 0.0
                    t = min(1.0, max(0.0, t))
                    val ex = xs[a] + t * dx - xs[i]
                    val ey = ys[a] + t * dy - ys[i]
                    val d = ex * ex + ey * ey
                    if (d > worst) { worst = d; wi = i }
                }
                if (worst > tol * tol) {
                    keep[wi] = true
                    stack.addLast(a to wi)
                    stack.addLast(wi to b)
                }
            }
            val out = p.filterIndexed { i, _ -> keep[i] }
            if (out.size <= maxPoints) return out
            tol *= 2
        }
    }

    /** The device's segment blob (format in src/segment_match.h). */
    fun encode(segs: List<DeviceSegment>): ByteArray {
        val out = ByteArrayOutputStream()
        fun le(v: Long, bytes: Int) {
            for (i in 0 until bytes) out.write(((v ushr (8 * i)) and 0xFF).toInt())
        }
        out.write("OTSG".toByteArray(Charsets.US_ASCII))
        out.write(1)   // version
        out.write(0)   // flags
        le(segs.size.toLong(), 2)
        for (s in segs) {
            // Whole code points only, up to NAME_BYTES.
            val name = ByteArrayOutputStream()
            var i = 0
            while (i < s.name.length) {
                val cp = s.name.codePointAt(i)
                val b = String(Character.toChars(cp)).toByteArray(Charsets.UTF_8)
                if (name.size() + b.size > NAME_BYTES) break
                name.write(b)
                i += Character.charCount(cp)
            }
            le(s.id, 8)
            le(max(0, s.prSeconds).toLong(), 4)
            le(max(0.0, roundAway(s.distanceMeters * 10)).toLong(), 4)
            le(roundAway(s.averageGrade * 10).coerceIn(-32768.0, 32767.0).toLong() and 0xFFFF, 2)
            le(s.points.size.toLong(), 2)
            out.write(name.size())
            out.write(name.toByteArray())
            for (q in s.points) {
                le(roundAway(q.lat * 1e5).toLong() and 0xFFFFFFFFL, 4)
                le(roundAway(q.lon * 1e5).toLong() and 0xFFFFFFFFL, 4)
            }
        }
        return out.toByteArray()
    }

    /**
     * The segments to send: rides only, in the order Strava listed them, each
     * with a polyline of at least two points.
     */
    fun deviceSegments(
        starred: List<StarredSegment>,
        polylines: Map<Long, String>,
        prs: Map<Long, Int> = emptyMap(),
    ): List<DeviceSegment> {
        val out = ArrayList<DeviceSegment>()
        for (s in starred) {
            if (!s.isRide || out.size >= MAX_SEGMENTS) continue
            val pts = polylines[s.id]?.let(CloudRoutes::decodePolyline) ?: continue
            val thin = simplify(pts)
            if (thin.size < 2) continue
            out += DeviceSegment(
                s.id, deviceName(s.name), s.prSeconds ?: prs[s.id] ?: 0,
                s.distanceMeters, s.averageGrade, thin,
            )
        }
        return out
    }

    /** Half away from zero, like Swift's rounded(), so both apps write the same bytes. */
    private fun roundAway(x: Double) = if (x >= 0) floor(x + 0.5) else -floor(-x + 0.5)

    private fun JsonObject.prim(k: String): JsonElement? = get(k)?.takeIf { it.isJsonPrimitive }
    private fun JsonObject.obj(k: String): JsonObject? = get(k)?.takeIf { it.isJsonObject }?.asJsonObject
    private fun JsonObject.num(k: String) = prim(k)?.takeIf { it.asJsonPrimitive.isNumber }?.asDouble
}
