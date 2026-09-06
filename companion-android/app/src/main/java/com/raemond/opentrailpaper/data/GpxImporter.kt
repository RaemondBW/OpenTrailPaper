package com.raemond.opentrailpaper.data

import org.xml.sax.Attributes
import org.xml.sax.InputSource
import org.xml.sax.SAXNotRecognizedException
import org.xml.sax.SAXNotSupportedException
import org.xml.sax.helpers.DefaultHandler
import java.io.InputStream
import java.io.StringReader
import javax.xml.XMLConstants
import javax.xml.parsers.SAXParserFactory
import kotlin.math.ceil

/** A route read out of somebody else's .gpx: geometry, a name, what it was planned for. */
data class ImportedRoute(
    val name: String,
    val points: List<LatLon>,
    /** The raw `<type>`, e.g. "racebike" or "cycling"; null when the file omits it. */
    val activityType: String?,
) {
    val distanceMeters: Double
        get() = (1 until points.size).sumOf { points[it - 1].distanceTo(points[it]) }

    /** Feeds the badge the summary card already draws. Blank when the file doesn't say. */
    val mode: String
        get() = when (activityType?.lowercase()) {
            "racebike", "cycling", "bike", "mtb", "touringbicycle", "gravel_bike",
            "mountainbike", "e_bike",
            -> "Cycling"
            else -> ""
        }
}

sealed interface ImportResult {
    data class Ok(val route: ImportedRoute) : ImportResult

    /** Not XML, or XML that isn't GPX. The MIME type lied. */
    data object NotGpx : ImportResult

    /** GPX, but with nothing in it that describes a path. */
    data object NoPoints : ImportResult
}

/**
 * Reads a GPX route into plain coordinates.
 *
 * The counterpart to [GpxExporter], and deliberately not a pass-through: the
 * firmware's scanner pairs any `lat="`/`lon="` it finds regardless of tag
 * (routes.cpp:319), so a file whose waypoints precede its track would have them
 * dragged into the polyline. Parsing here and re-emitting through [GpxExporter]
 * is what keeps that from happening — and drops `<ele>`/`<time>` on the way,
 * which is most of the file.
 *
 * SAX rather than [android.util.Xml] so this stays testable on the JVM, the same
 * reason the Overpass decoder uses Gson.
 */
object GpxImporter {

    /** The firmware's own cap (routes.cpp MAX_PTS). Decimating here saves BLE time. */
    const val MAX_POINTS = 4096

    /** Above this we stop collecting: a pathological file must not exhaust memory. */
    private const val HARD_LIMIT = 200_000

    fun parse(input: InputStream, fallbackName: String): ImportResult {
        val handler = Handler()
        try {
            val factory = SAXParserFactory.newInstance().apply {
                isNamespaceAware = false
                // A GPX is data, not a document that gets to name external
                // files, nor to expand one entity into gigabytes of text.
                // Neither may share the outer try: the second is a Xerces
                // feature name, Android's SAX (Expat-based) doesn't recognise it
                // and throws SAXNotRecognizedException, which would report every
                // real-device parse as NotGpx while the JVM tests (which use a
                // Xerces parser) stayed green. Secure processing is asked for
                // first for that same reason — it is the standard name, and a
                // throw on the Xerces one must not cost us a parser that would
                // have honoured it.
                //
                // Which is why neither is the fence that actually holds against
                // external entities: [Handler.resolveEntity] is, needing no
                // feature support from either parser.
                try {
                    setFeature(XMLConstants.FEATURE_SECURE_PROCESSING, true)
                    setFeature("http://apache.org/xml/features/nonvalidating/load-external-dtd", false)
                } catch (_: SAXNotRecognizedException) {
                } catch (_: SAXNotSupportedException) {
                }
            }
            factory.newSAXParser().parse(input, handler)
        } catch (_: Exception) {
            // A truncated file that already yielded a usable track is still a
            // route; anything else the check below rejects as not GPX.
        }
        if (!handler.sawGpx) return ImportResult.NotGpx

        val points = handler.trackPoints.ifEmpty { handler.routePoints }
        if (points.size < 2) return ImportResult.NoPoints

        return ImportResult.Ok(
            ImportedRoute(
                name = (handler.trackName ?: handler.metadataName ?: fallbackName).trim()
                    .ifEmpty { fallbackName },
                points = decimate(points, MAX_POINTS),
                activityType = handler.activityType?.trim()?.ifEmpty { null },
            ),
        )
    }

    /**
     * Every stride-th point, last one always kept.
     *
     * The same shape as the firmware's own halving (routes.cpp GpxScanner), done
     * here so a 10,000-point export doesn't spend minutes on the air only to be
     * thinned on arrival.
     */
    internal fun decimate(points: List<LatLon>, cap: Int): List<LatLon> {
        if (points.size <= cap) return points
        val stride = ceil(points.size.toDouble() / cap).toInt()
        val out = ArrayList<LatLon>(cap + 1)
        for (i in points.indices step stride) out.add(points[i])
        if (out.last() !== points.last()) out.add(points.last())
        return out
    }

    /**
     * Element names are matched against their parent, never on their own.
     *
     * `<name>` and `<type>` both appear inside `<author>` — Komoot writes
     * `<type>text/html</type>` for its own link, Strava writes the athlete in
     * `<metadata><author><name>`. Taking the first of either yields the author,
     * not the route.
     */
    private class Handler : DefaultHandler() {
        val trackPoints = ArrayList<LatLon>()
        val routePoints = ArrayList<LatLon>()
        var trackName: String? = null
        var metadataName: String? = null
        var activityType: String? = null
        var sawGpx = false

        private val stack = ArrayList<String>()
        private var text: StringBuilder? = null

        private fun parent() = stack.getOrNull(stack.size - 2)

        /**
         * Refuses every external entity, on whichever parser we were handed.
         *
         * The factory features above are advisory — one of them is a Xerces
         * name the device's parser throws on — so the guarantee lives here
         * instead, in plain SAX that both parsers honour: a file naming
         * file:///etc/passwd gets an empty string back, not the file. This
         * input arrives from any app on the phone through the VIEW/SEND
         * filters, so it cannot rest on a feature the target platform ignores.
         */
        override fun resolveEntity(publicId: String?, systemId: String?): InputSource =
            InputSource(StringReader(""))

        override fun startElement(uri: String?, l: String?, qName: String, a: Attributes?) {
            val name = qName.substringAfter(':').lowercase()
            stack.add(name)
            when (name) {
                "gpx" -> sawGpx = true
                "trkpt" -> point(a)?.let { if (trackPoints.size < HARD_LIMIT) trackPoints.add(it) }
                "rtept" -> point(a)?.let { if (routePoints.size < HARD_LIMIT) routePoints.add(it) }
                "name", "type" -> if (parent() in SCOPES) text = StringBuilder()
            }
        }

        override fun characters(ch: CharArray, start: Int, length: Int) {
            text?.append(ch, start, length)
        }

        override fun endElement(uri: String?, l: String?, qName: String) {
            val name = qName.substringAfter(':').lowercase()
            val value = text?.toString()
            text = null
            if (value != null) {
                when (name to parent()) {
                    "name" to "trk", "name" to "rte" -> if (trackName == null) trackName = value
                    "name" to "metadata" -> if (metadataName == null) metadataName = value
                    "type" to "trk", "type" to "rte" ->
                        if (activityType == null) activityType = value
                }
            }
            if (stack.isNotEmpty()) stack.removeAt(stack.size - 1)
        }

        private fun point(a: Attributes?): LatLon? {
            val lat = a?.getValue("lat")?.toDoubleOrNull() ?: return null
            val lon = a.getValue("lon")?.toDoubleOrNull() ?: return null
            if (lat !in -90.0..90.0 || lon !in -180.0..180.0) return null
            return LatLon(lat, lon)
        }

        private companion object {
            val SCOPES = setOf("trk", "rte", "metadata")
        }
    }
}
