package com.raemond.opentrailpaper.map

import kotlin.math.PI
import kotlin.math.atan
import kotlin.math.exp
import kotlin.math.ln
import kotlin.math.sin

/**
 * A fixed, zoom-independent Web Mercator space — the Android stand-in for
 * `MKMapPoint`, which the iOS decoder projects into once so nothing has to be
 * re-projected per frame.
 *
 * Having a fixed space matters twice over. Decoded geometry survives pans and
 * zooms untouched, and the screentones can be anchored to a GLOBAL grid rather
 * than to each tile — the thing that stops a visible seam appearing at every
 * hexagon boundary.
 *
 * Coordinates are stored in [android.graphics.Path], whose vertices are floats,
 * so geometry is kept RELATIVE to its own tile's origin. Absolute world values
 * run to 2^28 where a float's step is several metres; a few kilometres from the
 * tile origin the step is under a centimetre.
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
