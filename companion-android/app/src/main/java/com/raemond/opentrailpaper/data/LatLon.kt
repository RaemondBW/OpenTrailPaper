package com.raemond.opentrailpaper.data

import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt

/// A plain coordinate, so the decoders and the map builder never have to import
/// a map SDK. Converted to osmdroid's GeoPoint only at the drawing edge.
data class LatLon(val lat: Double, val lon: Double) {

    /** Great-circle distance in metres (haversine — the accuracy a bike track needs). */
    fun distanceTo(other: LatLon): Double {
        val r = 6_371_000.0
        val dLat = Math.toRadians(other.lat - lat)
        val dLon = Math.toRadians(other.lon - lon)
        val a = sin(dLat / 2) * sin(dLat / 2) +
            cos(Math.toRadians(lat)) * cos(Math.toRadians(other.lat)) *
            sin(dLon / 2) * sin(dLon / 2)
        return r * 2 * atan2(sqrt(a), sqrt(1 - a))
    }
}

/** A lat/lon bounding box, in the s/w/n/e order the whole codebase uses. */
data class BoundingBox(val south: Double, val west: Double, val north: Double, val east: Double) {
    val center get() = LatLon((south + north) / 2, (west + east) / 2)

    fun expanded(degrees: Double) =
        BoundingBox(south - degrees, west - degrees, north + degrees, east + degrees)

    fun intersects(other: BoundingBox): Boolean =
        !(other.east < west || other.west > east || other.north < south || other.south > north)

    companion object {
        /** The box enclosing every point, or null for an empty list. */
        fun around(points: List<LatLon>): BoundingBox? {
            if (points.isEmpty()) return null
            var s = 90.0; var w = 180.0; var n = -90.0; var e = -180.0
            for (p in points) {
                if (p.lat < s) s = p.lat
                if (p.lat > n) n = p.lat
                if (p.lon < w) w = p.lon
                if (p.lon > e) e = p.lon
            }
            return BoundingBox(s, w, n, e)
        }
    }
}
