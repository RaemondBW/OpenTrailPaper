package com.raemond.opentrailpaper.data

import java.util.Locale

/**
 * Serializes a route polyline to the GPX 1.1 `<trk>` the device's parser reads
 * (it scans lat="…"/lon="…" attribute pairs, order lat-before-lon).
 *
 * [Locale.US] on every format call, not the default: a phone set to a
 * comma-decimal locale would otherwise emit lat="37,7749", which the device's
 * scanner reads as 37 — a route that lands a hundred kilometres away.
 */
object GpxExporter {
    fun make(name: String, coordinates: List<LatLon>): String {
        val s = StringBuilder(coordinates.size * 48 + 256)
        s.append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n")
        s.append("<gpx version=\"1.1\" creator=\"OpenTrailPaper\" ")
        s.append("xmlns=\"http://www.topografix.com/GPX/1/1\">\n")
        s.append("<trk><name>").append(xmlEscape(name)).append("</name><trkseg>\n")
        for (c in coordinates) {
            s.append(
                String.format(
                    Locale.US,
                    "<trkpt lat=\"%.6f\" lon=\"%.6f\"></trkpt>",
                    c.lat,
                    c.lon,
                ),
            )
        }
        s.append("</trkseg></trk></gpx>")
        return s.toString()
    }

    private fun xmlEscape(s: String) =
        s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
}
