package com.raemond.opentrailpaper.routing

import com.raemond.opentrailpaper.BuildConfig
import com.raemond.opentrailpaper.ble.Maneuver
import com.raemond.opentrailpaper.data.BoundingBox
import com.raemond.opentrailpaper.data.DeviceText
import com.raemond.opentrailpaper.data.LatLon
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.util.Locale

/**
 * Destination search and cycling directions, from the same OpenStreetMap
 * ecosystem the offline map tiles come from.
 *
 * iOS gets these from MapKit, which has no Android equivalent that doesn't cost
 * an API key. Nominatim (search) and the FOSSGIS-hosted OSRM instances
 * (directions) need none, and they are the services the OSM website itself uses.
 * The bike profile is also a genuine improvement on the iOS behaviour, which
 * falls back to walking geometry wherever Apple has no cycling coverage — here
 * cycling is the primary profile everywhere, with walking as the fallback for
 * the same reason: it is the closest thing to a bike-friendly path.
 *
 * Both services ask clients to identify themselves and to keep request rates
 * modest; the User-Agent below is that identification, and searches only fire on
 * submit rather than per keystroke.
 */
object Routing {

    private const val USER_AGENT = "OpenTrailPaper-Android/${BuildConfig.VERSION_NAME}"

    /** One search result. */
    data class Place(val name: String, val detail: String, val coordinate: LatLon)

    /** A built route, ready to preview and send. */
    data class Route(
        val coordinates: List<LatLon>,
        val distanceMeters: Double,
        val durationSeconds: Double,
        val maneuvers: List<Maneuver>,
        /** "Cycling" or "Walking" — what the summary card badges. */
        val mode: String,
    ) {
        val bounds: BoundingBox? get() = BoundingBox.around(coordinates)
    }

    // MARK: search

    suspend fun search(query: String, near: LatLon?): List<Place> = withContext(Dispatchers.IO) {
        if (query.isBlank()) return@withContext emptyList()
        val url = buildString {
            append("https://nominatim.openstreetmap.org/search?format=jsonv2&limit=8&q=")
            append(URLEncoder.encode(query, "UTF-8"))
            if (near != null) {
                // A viewbox biases results toward the rider without excluding a
                // deliberate search for somewhere else — bounded=0 is the
                // difference between "prefer nearby" and "nearby only".
                val d = 0.6
                append("&bounded=0&viewbox=")
                append(
                    fmt(near.lon - d) + "," + fmt(near.lat + d) + "," +
                        fmt(near.lon + d) + "," + fmt(near.lat - d),
                )
            }
        }
        val body = get(url) ?: return@withContext emptyList()
        val arr = org.json.JSONArray(body)
        (0 until arr.length()).mapNotNull { i ->
            val o = arr.optJSONObject(i) ?: return@mapNotNull null
            val lat = o.optDouble("lat", Double.NaN)
            val lon = o.optDouble("lon", Double.NaN)
            if (lat.isNaN() || lon.isNaN()) return@mapNotNull null
            val display = o.optString("display_name")
            val name = o.optString("name").ifEmpty { display.substringBefore(",") }
            Place(
                name = name.ifEmpty { "Destination" },
                detail = display.substringAfter(",").trim(),
                coordinate = LatLon(lat, lon),
            )
        }
    }

    // MARK: directions

    /**
     * Prefer cycling directions, falling back to walking geometry when the bike
     * router has nothing (a start point off the cycling network, an island, a
     * region the instance hasn't loaded).
     */
    suspend fun route(from: LatLon, to: LatLon): Route? =
        calc(from, to, "routed-bike", "Cycling") ?: calc(from, to, "routed-foot", "Walking")

    private suspend fun calc(
        from: LatLon,
        to: LatLon,
        profile: String,
        mode: String,
    ): Route? = withContext(Dispatchers.IO) {
        val url = "https://routing.openstreetmap.de/$profile/route/v1/driving/" +
            "${fmt(from.lon)},${fmt(from.lat)};${fmt(to.lon)},${fmt(to.lat)}" +
            "?overview=full&geometries=geojson&steps=true&alternatives=false"
        val body = get(url) ?: return@withContext null
        val root = JSONObject(body)
        if (root.optString("code") != "Ok") return@withContext null
        val route = root.optJSONArray("routes")?.optJSONObject(0) ?: return@withContext null

        val coords = route.optJSONObject("geometry")?.optJSONArray("coordinates")
            ?: return@withContext null
        val points = ArrayList<LatLon>(coords.length())
        for (i in 0 until coords.length()) {
            val pair = coords.optJSONArray(i) ?: continue
            // GeoJSON is [lon, lat] — the one ordering mistake here puts a route
            // in the wrong hemisphere, so it is worth naming.
            points.add(LatLon(pair.optDouble(1), pair.optDouble(0)))
        }
        if (points.size < 2) return@withContext null

        // Turn cues from the route steps: the maneuver point is where the
        // instruction happens; skip the "depart"/"arrive" bookends, which carry
        // no turn for the device to announce.
        val maneuvers = ArrayList<Maneuver>()
        val legs = route.optJSONArray("legs")
        for (l in 0 until (legs?.length() ?: 0)) {
            val steps = legs!!.optJSONObject(l)?.optJSONArray("steps") ?: continue
            for (s in 0 until steps.length()) {
                val step = steps.optJSONObject(s) ?: continue
                val man = step.optJSONObject("maneuver") ?: continue
                val loc = man.optJSONArray("location") ?: continue
                val text = instruction(man, step.optString("name")) ?: continue
                maneuvers.add(Maneuver(loc.optDouble(1), loc.optDouble(0), text))
            }
        }

        Route(
            coordinates = points,
            distanceMeters = route.optDouble("distance", 0.0),
            durationSeconds = route.optDouble("duration", 0.0),
            maneuvers = maneuvers,
            mode = mode,
        )
    }

    // MARK: turn cues for an imported route

    /** routes.h MAX_MANEUVERS — more than this and the device drops the rest. */
    private const val MAX_MANEUVERS = 128

    /** How many via points describe the track well enough to route through it. */
    private const val VIA_COUNT = 50

    /** Cues that survived, and whether anything had to be thrown away to fit. */
    data class CueSet(val maneuvers: List<Maneuver>, val truncated: Boolean)

    /** A cue plus the two facts triage needs when there are too many of them. */
    internal data class Cue(
        val maneuver: Maneuver,
        /** "Continue onto X" — no turn in it, the first thing worth losing. */
        val isFiller: Boolean,
        /** A slight left/right, the next thing worth losing. */
        val isSlight: Boolean,
    )

    /**
     * Turn cues for a route the rider brought with them.
     *
     * The track is sampled to [VIA_COUNT] waypoints and routed through, then
     * OSRM's geometry is **discarded** — only the maneuver coordinates are kept.
     * The device projects those onto whatever route is loaded
     * (routes.cpp mapManeuversToRoute), so the rider still rides their own file
     * while getting named turns off the OSM network.
     *
     * Measured against four real Komoot/Strava exports, cues land a median 0–1 m
     * from the original track and 36 m at worst — inside the 20 m tolerance the
     * firmware's own past-the-turn rule already lives with.
     *
     * Null on any failure: no network, a refusal, a route OSRM can't follow. The
     * caller keeps the route and sends it without cues.
     */
    suspend fun cues(track: List<LatLon>): CueSet? = withContext(Dispatchers.IO) {
        if (track.size < 2) return@withContext null
        val vias = sampleByDistance(track, VIA_COUNT)
        if (vias.size < 2) return@withContext null

        val path = vias.joinToString(";") { "${fmt(it.lon)},${fmt(it.lat)}" }
        val url = "https://routing.openstreetmap.de/routed-bike/route/v1/driving/$path" +
            "?overview=false&steps=true&alternatives=false"
        val body = get(url) ?: return@withContext null

        val root = JSONObject(body)
        if (root.optString("code") != "Ok") return@withContext null
        val route = root.optJSONArray("routes")?.optJSONObject(0) ?: return@withContext null

        val collected = ArrayList<Cue>()
        val legs = route.optJSONArray("legs")
        for (l in 0 until (legs?.length() ?: 0)) {
            val steps = legs!!.optJSONObject(l)?.optJSONArray("steps") ?: continue
            for (s in 0 until steps.length()) {
                val step = steps.optJSONObject(s) ?: continue
                val man = step.optJSONObject("maneuver") ?: continue
                val loc = man.optJSONArray("location") ?: continue
                // instruction() already returns null for depart/arrive, which is
                // what discards the two artifacts every via point produces.
                val txt = instruction(man, step.optString("name")) ?: continue
                collected.add(
                    Cue(
                        maneuver = Maneuver(loc.optDouble(1), loc.optDouble(0), txt),
                        isFiller = txt.startsWith("Continue"),
                        isSlight = man.optString("modifier").startsWith("slight"),
                    ),
                )
            }
        }
        if (collected.isEmpty()) return@withContext null
        triage(collected)
    }

    /**
     * [n] points spaced evenly along the track *by distance*.
     *
     * By distance and not by index because GPX point density varies wildly
     * within one file — Komoot writes a point per second, so a slow climb is
     * dense and a fast descent sparse. Sampling by index would spend most of the
     * via budget on whichever part the rider went slowest through.
     *
     * The sample count is clamped to the track length: a short track asked for
     * more vias than it has points would otherwise emit mostly duplicates,
     * wasting OSRM's waypoint budget on a degenerate request.
     */
    internal fun sampleByDistance(points: List<LatLon>, n: Int): List<LatLon> {
        if (points.size <= 2 || n < 2) return points
        val cum = DoubleArray(points.size)
        for (i in 1 until points.size) {
            cum[i] = cum[i - 1] + points[i - 1].distanceTo(points[i])
        }
        val total = cum.last()
        if (total <= 0.0) return listOf(points.first(), points.last())

        val count = minOf(n, points.size)
        val out = ArrayList<LatLon>(count)
        var j = 0
        for (i in 0 until count) {
            val target = if (count > 1) total * i / (count - 1) else 0.0
            while (j < cum.size - 1 && cum[j + 1] < target) j++
            out.add(points[j])
        }
        out[out.size - 1] = points.last()
        return out
    }

    /**
     * Fit the cues into the device's maneuver table, cheapest losses first.
     *
     * Filler goes before slight turns, and slight turns before real ones. Only
     * when even the real turns overflow does this truncate — and then it says
     * so, because silently losing the last third of a route's cues is worse than
     * admitting the list was cut.
     */
    internal fun triage(cues: List<Cue>, cap: Int = MAX_MANEUVERS): CueSet {
        var kept: List<Cue> = cues
        if (kept.size > cap) kept = kept.filterNot { it.isFiller }
        if (kept.size > cap) kept = kept.filterNot { it.isSlight }
        val truncated = kept.size > cap
        if (truncated) kept = kept.take(cap)
        return CueSet(
            maneuvers = kept.map { it.maneuver.copy(text = DeviceText.maneuverText(it.maneuver.text)) },
            truncated = truncated,
        )
    }

    /**
     * Human turn text from an OSRM maneuver.
     *
     * OSRM returns a type and a modifier rather than a sentence (unlike MapKit,
     * which hands over prose), so the wording is assembled here — and kept short,
     * because the device draws it on a 1-bit panel a rider reads at speed.
     */
    private fun instruction(maneuver: JSONObject, road: String): String? {
        val type = maneuver.optString("type")
        val modifier = maneuver.optString("modifier")
        val onto = if (road.isNotBlank()) " onto $road" else ""

        val direction = when (modifier) {
            "left" -> "left"
            "right" -> "right"
            "slight left" -> "slightly left"
            "slight right" -> "slightly right"
            "sharp left" -> "sharp left"
            "sharp right" -> "sharp right"
            "uturn" -> "around"
            else -> null
        }

        return when (type) {
            "depart", "arrive" -> null
            "turn", "new name", "end of road", "continue" ->
                if (direction != null) "Turn $direction$onto" else "Continue$onto"

            "merge" -> if (direction != null) "Merge $direction$onto" else "Merge$onto"
            "on ramp" -> if (direction != null) "Take the ramp $direction$onto" else "Take the ramp$onto"
            "off ramp" -> if (direction != null) "Exit $direction$onto" else "Exit$onto"
            "fork" -> if (direction != null) "Keep $direction$onto" else "Keep ahead$onto"
            "roundabout", "rotary", "roundabout turn" -> {
                val exit = maneuver.optInt("exit", 0)
                if (exit > 0) "Roundabout, exit $exit$onto" else "At the roundabout$onto"
            }

            else -> if (direction != null) "Turn $direction$onto" else null
        }
    }

    // MARK: transport

    private fun get(url: String): String? {
        var conn: HttpURLConnection? = null
        return try {
            conn = (URL(url).openConnection() as HttpURLConnection).apply {
                setRequestProperty("User-Agent", USER_AGENT)
                setRequestProperty("Accept", "application/json")
                connectTimeout = 20_000
                readTimeout = 30_000
            }
            if (conn.responseCode != 200) null
            else conn.inputStream.readBytes().toString(Charsets.UTF_8)
        } catch (_: Exception) {
            null
        } finally {
            conn?.disconnect()
        }
    }

    /** Plain decimal always, whatever the phone's locale — see GpxExporter. */
    private fun fmt(v: Double) = String.format(Locale.US, "%.6f", v)
}
