package com.raemond.opentrailpaper.map

import kotlin.math.PI
import kotlin.math.atan
import kotlin.math.exp
import kotlin.math.ln
import kotlin.math.sin

/**
 * A fixed, zoom-independent Web Mercator space — the Android stand-in for
 * `MKMapPoint`.
 *
 * Being zoom-independent is the point: a position converts once and then any
 * zoom is a single multiply away, which is what lets [MapSnapshotter] lay a ride
 * over stitched raster tiles and [com.raemond.opentrailpaper.ui.OsmMap] work out
 * the zoom and centre that frame a bounding box above a floating card.
 */
object MercatorWorld {
    /** World size in units. A power of two, so tone-cell quantisation is exact. */
    const val WORLD = 268_435_456.0   // 2^28

    /** The latitude Web Mercator is defined up to; beyond it y runs away. */
    private const val MAX_LAT = 85.05112877980659

    fun x(lon: Double): Double = (lon + 180.0) / 360.0 * WORLD

    fun y(lat: Double): Double {
        val clamped = lat.coerceIn(-MAX_LAT, MAX_LAT)
        val s = sin(clamped * PI / 180.0)
        return (0.5 - ln((1 + s) / (1 - s)) / (4 * PI)) * WORLD
    }

    /** Inverse of [y] — used to place the second probe when calibrating the
     *  world→screen transform against the live map. */
    fun latFromY(worldY: Double): Double {
        val n = PI - 2.0 * PI * worldY / WORLD
        return 180.0 / PI * atan(0.5 * (exp(n) - exp(-n)))
    }
}
