package com.raemond.opentrailpaper.map

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import androidx.compose.ui.graphics.toArgb
import com.raemond.opentrailpaper.ui.Palette

/**
 * The two pieces of map furniture the app draws itself.
 *
 * osmdroid ships a default person sprite and a green push-pin, and both look like
 * a different application. Drawn here for the same reason the synced check is:
 * they are small, they have to hold up over any tile underneath, and drawing them
 * keeps them in the app's palette without a set of density-specific PNGs to
 * maintain.
 */
object MapMarkers {

    private val cache = HashMap<String, Bitmap>()

    /**
     * "You are here": a blue dot with a white ring and a soft shadow.
     *
     * Blue rather than the app's vermilion on purpose — the accent is already the
     * route line, and a rider glancing at the map needs those two to be instantly
     * different things. Blue is also what the same dot is on the iOS companion.
     */
    fun locationDot(density: Float): Bitmap = cache.getOrPut("dot-$density") {
        val d = 26f * density
        val c = d / 2
        val bitmap = Bitmap.createBitmap(d.toInt(), d.toInt(), Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bitmap)
        val paint = Paint(Paint.ANTI_ALIAS_FLAG)

        paint.color = Color.argb(40, 0, 0, 0)
        canvas.drawCircle(c, c + 0.5f * density, 9f * density, paint)
        paint.color = Color.WHITE
        canvas.drawCircle(c, c, 8.5f * density, paint)
        paint.color = LOCATION_BLUE
        canvas.drawCircle(c, c, 6f * density, paint)
        bitmap
    }

    /** The destination pin: a vermilion teardrop with a hole punched in it. */
    fun destinationPin(density: Float): Bitmap = cache.getOrPut("pin-$density") {
        val w = 26f * density
        val h = 34f * density
        val bitmap = Bitmap.createBitmap(w.toInt(), h.toInt(), Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bitmap)
        val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        val r = 11f * density
        val cx = w / 2
        val cy = r + 1f * density

        paint.color = Color.argb(45, 0, 0, 0)
        canvas.drawCircle(cx, cy + 1.5f * density, r, paint)

        // Head and tail as one shape, so the join has no seam to show.
        paint.color = Palette.accent.toArgb()
        canvas.drawCircle(cx, cy, r, paint)
        val tail = Path().apply {
            moveTo(cx - 6.5f * density, cy + 8f * density)
            lineTo(cx, h - 1f * density)
            lineTo(cx + 6.5f * density, cy + 8f * density)
            close()
        }
        canvas.drawPath(tail, paint)

        paint.color = Color.WHITE
        canvas.drawCircle(cx, cy, 4f * density, paint)
        bitmap
    }

    /**
     * A mesh neighbour: a green pin with radio waves, or a hollow grey one where
     * the sender deliberately blurred the position.
     *
     * Hollow is not decoration. Meshtastic lets a node publish a rounded-off
     * coordinate, and a solid pin on a coordinate the sender rounded off would be
     * claiming more than they gave.
     */
    fun meshNodePin(density: Float, imprecise: Boolean): Bitmap =
        cache.getOrPut("mesh-$density-$imprecise") {
            val w = 26f * density
            val h = 34f * density
            val bitmap = Bitmap.createBitmap(w.toInt(), h.toInt(), Bitmap.Config.ARGB_8888)
            val canvas = Canvas(bitmap)
            val paint = Paint(Paint.ANTI_ALIAS_FLAG)
            val r = 11f * density
            val cx = w / 2
            val cy = r + 1f * density
            val body = if (imprecise) Palette.faint.toArgb() else Palette.good.toArgb()

            paint.color = Color.argb(45, 0, 0, 0)
            canvas.drawCircle(cx, cy + 1.5f * density, r, paint)

            val tail = Path().apply {
                moveTo(cx - 6.5f * density, cy + 8f * density)
                lineTo(cx, h - 1f * density)
                lineTo(cx + 6.5f * density, cy + 8f * density)
                close()
            }
            if (imprecise) {
                // Outline only, filled with paper so the tile beneath does not
                // read through and make it look like a solid pin after all.
                paint.color = Palette.paper.toArgb()
                canvas.drawCircle(cx, cy, r, paint)
                canvas.drawPath(tail, paint)
                paint.style = Paint.Style.STROKE
                paint.strokeWidth = 2f * density
                paint.color = body
                canvas.drawCircle(cx, cy, r - 1f * density, paint)
                paint.style = Paint.Style.FILL
            } else {
                paint.color = body
                canvas.drawCircle(cx, cy, r, paint)
                canvas.drawPath(tail, paint)
            }

            // Three arcs and a dot — an antenna radiating, at pin scale.
            paint.color = if (imprecise) body else Color.WHITE
            paint.style = Paint.Style.STROKE
            paint.strokeWidth = 1.6f * density
            for (i in 1..2) {
                val rr = (2.6f + i * 2.6f) * density
                canvas.drawArc(cx - rr, cy - rr, cx + rr, cy + rr, -140f, 100f, false, paint)
                canvas.drawArc(cx - rr, cy - rr, cx + rr, cy + rr, 40f, 100f, false, paint)
            }
            paint.style = Paint.Style.FILL
            canvas.drawCircle(cx, cy, 1.9f * density, paint)
            bitmap
        }

    /** iOS's user-location blue, so the two companions agree on what a rider is. */
    private val LOCATION_BLUE = Color.rgb(0x0A, 0x84, 0xFF)
}
