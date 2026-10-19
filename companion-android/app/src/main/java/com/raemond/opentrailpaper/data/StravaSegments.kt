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
    /** Metres, one per point; null = none. */
    val elevations: List<Double>? = null,
)

/**
 * A segment's line as the device keeps it: at most [StravaSegments.MAX_POINTS]
 * points, with the altitude at each when Strava had one. Also what the app
 * caches per id.
 */
data class SegmentGeometry(val points: List<LatLon>, val altitudes: List<Double>?)

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
    /** The newest blob version the apps write; v2 carries elevation. */
    const val BLOB_VERSION = 2
    /** v2 elevation: decimetres above this many metres below sea level. */
    const val ELEV_OFFSET_M = 1000.0
    /** How much a metre of height counts against a metre of line in the cut (see [simplify]). */
    const val ELEV_WEIGHT = 10.0

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
     * GET /api/v3/segments/{id}/streams?keys=latlng,altitude,distance&key_by_type=true:
     * {"latlng": {"data": [[lat, lon], ...]}, "altitude": {"data": [m, ...]}, ...},
     * or the same streams as an array of {"type": ..., "data": [...]}. Null
     * without a usable latlng; the altitude only when it lines up with it.
     */
    fun parseStreams(text: String): Pair<List<LatLon>, List<Double>?>? {
        val json = runCatching { JsonParser.parseString(text) }.getOrNull() ?: return null
        val byType = HashMap<String, com.google.gson.JsonArray>()
        if (json.isJsonObject) {
            for ((k, v) in json.asJsonObject.entrySet()) {
                v.takeIf { it.isJsonObject }?.asJsonObject?.get("data")
                    ?.takeIf { it.isJsonArray }?.let { byType[k] = it.asJsonArray }
            }
        } else if (json.isJsonArray) {
            for (e in json.asJsonArray) {
                val o = e.takeIf { it.isJsonObject }?.asJsonObject ?: continue
                val t = o.prim("type")?.asString ?: continue
                o.get("data")?.takeIf { it.isJsonArray }?.let { byType[t] = it.asJsonArray }
            }
        }
        val ll = byType["latlng"] ?: return null
        val pts = ll.mapNotNull { e ->
            val a = e.takeIf { it.isJsonArray }?.asJsonArray?.takeIf { it.size() >= 2 } ?: return@mapNotNull null
            val la = runCatching { a[0].asDouble }.getOrNull() ?: return@mapNotNull null
            val lo = runCatching { a[1].asDouble }.getOrNull() ?: return@mapNotNull null
            LatLon(la, lo)
        }
        if (pts.size < 2 || pts.size != ll.size()) return null
        val alt = byType["altitude"]?.mapNotNull { e ->
            e.takeIf { it.isJsonPrimitive && it.asJsonPrimitive.isNumber }?.asDouble
        }
        return pts to alt?.takeIf { it.size == pts.size }
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

    /** The line cut to [maxPoints], without its elevation. */
    fun simplify(pts: List<LatLon>, maxPoints: Int = MAX_POINTS): List<LatLon> =
        simplify(pts, null, maxPoints).points

    /**
     * Cuts the line and its altitudes together, keeping the same points of
     * both. Douglas-Peucker in three dimensions — metres east, north, and
     * height x [ELEV_WEIGHT] — with the tolerance doubling from 2 m until the
     * line fits. Not points spaced evenly along it: the device matches the
     * rider against this line, so its bends must stay where the road bends,
     * and the profile must keep its grade changes (a steep ramp on a straight
     * road is invisible in 2D). Drops repeated points first; keeps both ends.
     * Same as StravaSegments.swift.
     */
    fun simplify(pts: List<LatLon>, altitudes: List<Double>?, maxPoints: Int = MAX_POINTS): SegmentGeometry {
        val alts = altitudes?.takeIf { it.size == pts.size }
        val idx = ArrayList<Int>()
        for (i in pts.indices) if (idx.isEmpty() || pts[idx.last()] != pts[i]) idx.add(i)
        val p = idx.map { pts[it] }
        if (p.size <= 2) return SegmentGeometry(p, alts?.let { a -> idx.map { a[it] } })
        val kx = 111_320.0 * cos(Math.toRadians(p[0].lat))
        val ky = 110_540.0
        val xs = DoubleArray(p.size) { (p[it].lon - p[0].lon) * kx }
        val ys = DoubleArray(p.size) { (p[it].lat - p[0].lat) * ky }
        val zs = DoubleArray(p.size) { if (alts != null) alts[idx[it]] * ELEV_WEIGHT else 0.0 }
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
                val dz = zs[b] - zs[a]
                val len2 = dx * dx + dy * dy + dz * dz
                var worst = -1.0
                var wi = a
                for (i in a + 1 until b) {
                    var t = if (len2 > 0) ((xs[i] - xs[a]) * dx + (ys[i] - ys[a]) * dy + (zs[i] - zs[a]) * dz) / len2 else 0.0
                    t = min(1.0, max(0.0, t))
                    val ex = xs[a] + t * dx - xs[i]
                    val ey = ys[a] + t * dy - ys[i]
                    val ez = zs[a] + t * dz - zs[i]
                    val d = ex * ex + ey * ey + ez * ez
                    if (d > worst) { worst = d; wi = i }
                }
                if (worst > tol * tol) {
                    keep[wi] = true
                    stack.addLast(a to wi)
                    stack.addLast(wi to b)
                }
            }
            val kept = p.indices.filter { keep[it] }
            if (kept.size <= maxPoints) {
                return SegmentGeometry(kept.map { p[it] }, alts?.let { a -> kept.map { a[idx[it]] } })
            }
            tol *= 2
        }
    }

    /**
     * The device's segment blob (format in src/segment_match.h). [version] 1
     * is for firmware that predates elevation: the same lines, no heights.
     */
    fun encode(segs: List<DeviceSegment>, version: Int = BLOB_VERSION): ByteArray {
        val out = ByteArrayOutputStream()
        fun le(v: Long, bytes: Int) {
            for (i in 0 until bytes) out.write(((v ushr (8 * i)) and 0xFF).toInt())
        }
        out.write("OTSG".toByteArray(Charsets.US_ASCII))
        out.write(version)
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
            val elev = s.elevations?.takeIf { it.size == s.points.size }
            le(s.id, 8)
            le(max(0, s.prSeconds).toLong(), 4)
            le(max(0.0, roundAway(s.distanceMeters * 10)).toLong(), 4)
            le(roundAway(s.averageGrade * 10).coerceIn(-32768.0, 32767.0).toLong() and 0xFFFF, 2)
            le(s.points.size.toLong(), 2)
            if (version >= 2) out.write(if (elev != null) 1 else 0)
            out.write(name.size())
            out.write(name.toByteArray())
            s.points.forEachIndexed { k, q ->
                le(roundAway(q.lat * 1e5).toLong() and 0xFFFFFFFFL, 4)
                le(roundAway(q.lon * 1e5).toLong() and 0xFFFFFFFFL, 4)
                if (version >= 2) {
                    val dm = elev?.let { roundAway((it[k] + ELEV_OFFSET_M) * 10) } ?: 0.0
                    le(dm.coerceIn(0.0, 65535.0).toLong(), 2)
                }
            }
        }
        return out.toByteArray()
    }

    /**
     * The segments to send: rides only, in the order Strava listed them, each
     * with a line of at least two points.
     */
    fun deviceSegments(
        starred: List<StarredSegment>,
        geometry: Map<Long, SegmentGeometry>,
        prs: Map<Long, Int> = emptyMap(),
    ): List<DeviceSegment> {
        val out = ArrayList<DeviceSegment>()
        for (s in starred) {
            if (!s.isRide || out.size >= MAX_SEGMENTS) continue
            val g = geometry[s.id] ?: continue
            if (g.points.size < 2 || g.points.size > MAX_POINTS) continue
            out += DeviceSegment(
                s.id, deviceName(s.name), s.prSeconds ?: prs[s.id] ?: 0,
                s.distanceMeters, s.averageGrade, g.points, g.altitudes,
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
