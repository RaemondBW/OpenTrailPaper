package com.raemond.opentrailpaper.map

import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.Point
import androidx.compose.ui.graphics.toArgb
import com.raemond.opentrailpaper.data.LatLon
import com.raemond.opentrailpaper.ui.Palette
import org.osmdroid.util.GeoPoint
import org.osmdroid.views.Projection
import org.osmdroid.views.overlay.Overlay

/**
 * Flat hexagons: the ground this phone or the device holds, and the area being
 * selected for download.
 *
 * These used to sit under an opaque e-ink painting of each area (see
 * [EInkTileStore] for why that is gone), so they only had to hint at an edge.
 * Now they ARE the coverage, and are drawn to be seen.
 *
 * Drawn as one overlay rather than an osmdroid Polygon per hex. A large
 * selection is several hundred cells, and several hundred overlays means several
 * hundred objects re-added to the map every time the device reports one more
 * tile — which is exactly the churn that made the Maps screen stutter mid-download.
 */
class HexOverlay(
    private val hexes: List<Hex>,
    /** Screen pixels per dp — the strokes below are quoted in dp. */
    private val density: Float,
) : Overlay() {

    /** One hexagon and how it should read. */
    /** [badge]: the centre badge of an on-device hex, or null for none. */
    class Hex(val outline: List<LatLon>, val style: Style, val badge: HexBadge.Badge? = null)

    enum class Style {
        /** In the drawn box, queued to download. */
        SELECTION_PENDING,

        /** Built and sent this run. */
        SELECTION_DONE,

        /** Tapped out of the selection. */
        SELECTION_EXCLUDED,

        /** Downloaded on this phone. */
        OUTLINE_PHONE,

        /** As above, and the device has it too. */
        OUTLINE_SYNCED,

        /** On the device, update available — ochre and hatched. */
        OUTLINE_UPDATE,

        /** Selected, already on the device and current. */
        SELECTION_CURRENT,

        /** Selected, already on the device, update available. */
        SELECTION_UPDATE,

        /** Nothing drawn but the badge (a selected on-device hex). */
        BADGE_ONLY,

        /**
         * A gap in the maps, so it has to read as "look here" — the accent,
         * barely tinted: these sit under the route line, which must stay the
         * most legible thing on the screen.
         */
        MISSING,
        ;

        val fill: Int
            get() = when (this) {
                SELECTION_PENDING -> Palette.accent.toArgb().withAlpha(0.16f)
                SELECTION_DONE -> Palette.good.toArgb().withAlpha(0.22f)
                SELECTION_EXCLUDED -> Palette.muted.toArgb().withAlpha(0.08f)
                // At 0.14 over Palette.faint these were tuned to whisper beneath
                // the paper fill of a painted area, and with that gone they
                // vanished into the base map's green terrain — visible only where
                // a hexagon happened to cross water.
                OUTLINE_PHONE -> Palette.muted.toArgb().withAlpha(0.22f)
                OUTLINE_SYNCED -> Palette.good.toArgb().withAlpha(0.30f)
                OUTLINE_UPDATE -> Palette.update.toArgb().withAlpha(0.22f)
                SELECTION_CURRENT -> Palette.good.toArgb().withAlpha(0.34f)
                SELECTION_UPDATE -> Palette.update.toArgb().withAlpha(0.30f)
                BADGE_ONLY -> Color.TRANSPARENT
                MISSING -> Palette.accent.toArgb().withAlpha(0.10f)
            }

        val stroke: Int
            get() = when (this) {
                SELECTION_PENDING -> Palette.accent.toArgb()
                SELECTION_DONE -> Palette.good.toArgb()
                SELECTION_EXCLUDED -> Palette.muted.toArgb().withAlpha(0.55f)
                OUTLINE_PHONE -> Palette.muted.toArgb()
                OUTLINE_SYNCED -> Palette.good.toArgb()
                OUTLINE_UPDATE, SELECTION_UPDATE -> Palette.update.toArgb()
                SELECTION_CURRENT -> Palette.good.toArgb()
                BADGE_ONLY -> Color.TRANSPARENT
                MISSING -> Palette.accent.toArgb().withAlpha(0.7f)
            }

        /** Update available is hatched as well as coloured, so it never
         *  depends on telling ochre from green. */
        val hatched: Boolean get() = this == OUTLINE_UPDATE || this == SELECTION_UPDATE

        /** Selected hexes are outlined heavier than coverage. */
        val strokeDp: Float
            get() = when (this) {
                SELECTION_PENDING, SELECTION_DONE, SELECTION_CURRENT, SELECTION_UPDATE -> 3.5f
                else -> 2f
            }
    }

    private val hatchPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 1.4f * density
    }

    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val strokePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2f * density
    }
    private val path = Path()
    private val point = Point()

    override fun draw(canvas: Canvas, projection: Projection) {
        for (hex in hexes) {
            if (hex.outline.size < 3) continue
            path.rewind()
            var cx = 0f
            var cy = 0f
            for ((i, c) in hex.outline.withIndex()) {
                projection.toPixels(GeoPoint(c.lat, c.lon), point)
                if (i == 0) path.moveTo(point.x.toFloat(), point.y.toFloat())
                else path.lineTo(point.x.toFloat(), point.y.toFloat())
                cx += point.x
                cy += point.y
            }
            path.close()
            fillPaint.color = hex.style.fill
            strokePaint.color = hex.style.stroke
            strokePaint.strokeWidth = hex.style.strokeDp * density
            canvas.drawPath(path, fillPaint)
            if (hex.style.hatched) {
                canvas.save()
                canvas.clipPath(path)
                val b = android.graphics.RectF()
                path.computeBounds(b, true)
                hatchPaint.color = hex.style.stroke.withAlpha(0.55f)
                val step = 9f * density
                var x = b.left - b.height()
                while (x < b.right) {
                    canvas.drawLine(x, b.bottom, x + b.height(), b.top, hatchPaint)
                    x += step
                }
                canvas.restore()
            }
            canvas.drawPath(path, strokePaint)
            hex.badge?.let {
                HexBadge.draw(canvas, cx / hex.outline.size, cy / hex.outline.size, density, it)
            }
        }
    }
}

/**
 * The badge in the middle of an on-device hex: a green check (current) or an
 * ochre up-arrow (update available), with a small water drop beside it when the
 * firmware has the POI layer — filled ink when the device has the hex's POIs,
 * hollow when it has none or they are stale.
 *
 * Drawn rather than tinted from a vector asset: the mark has to hold its colour
 * over whatever ground the base map puts under it, and the white ring is what
 * keeps it legible on the dark parts. 14 dp across — it is a status badge on a
 * ~5.6 km hexagon, not a pin.
 */
object HexBadge {
    data class Badge(val update: Boolean, val poi: HexPoiMark)

    private const val D = 14f
    private val ringPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = Color.WHITE
    }
    private val discPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val markPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        color = Color.WHITE
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }
    private val dropFill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val dropStroke = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }

    fun draw(canvas: Canvas, cx: Float, cy: Float, density: Float, b: Badge) {
        val d = D * density
        val k = d / 22f      // the marks below were drawn at 22
        val r = d / 2
        // Badge left of centre when a drop sits beside it, so the pair is centred.
        val dropW = if (b.poi == HexPoiMark.NONE) 0f else 11f * density
        val bx = cx - (if (dropW > 0) (dropW + 2 * density) / 2 else 0f)
        markPaint.strokeWidth = 2.4f * k
        discPaint.color = (if (b.update) Palette.update else Palette.good).toArgb()
        canvas.drawCircle(bx, cy, r, ringPaint)
        canvas.drawCircle(bx, cy, r - 1.5f * k, discPaint)
        val left = bx - r
        val top = cy - r
        val mark = Path().apply {
            if (b.update) {          // up arrow: "a newer version is available"
                moveTo(left + 11f * k, top + 16.5f * k); lineTo(left + 11f * k, top + 5.5f * k)
                moveTo(left + 6.8f * k, top + 9.6f * k); lineTo(left + 11f * k, top + 5.5f * k)
                lineTo(left + 15.2f * k, top + 9.6f * k)
            } else {
                moveTo(left + 6.2f * k, top + 11.4f * k)
                lineTo(left + 9.6f * k, top + 14.8f * k)
                lineTo(left + 15.8f * k, top + 7.4f * k)
            }
        }
        canvas.drawPath(mark, markPaint)
        if (dropW > 0) {
            // A water drop: round belly, pointed top.
            val x0 = bx + r + 2 * density
            val w = dropW - 2 * density
            val rr = w / 2
            val belly = cy + r - rr - 1 * density
            val drop = Path().apply {
                moveTo(x0 + rr, cy - r + 1 * density)
                cubicTo(x0 + rr, cy - r + 3 * density, x0 + w, cy - 1 * density, x0 + w, belly)
                arcTo(android.graphics.RectF(x0, belly - rr, x0 + w, belly + rr), 0f, 180f)
                cubicTo(x0, cy - 1 * density, x0 + rr, cy - r + 3 * density, x0 + rr, cy - r + 1 * density)
                close()
            }
            dropStroke.color = Color.WHITE
            dropStroke.strokeWidth = 3f * density
            canvas.drawPath(drop, dropStroke)            // halo
            dropFill.color = if (b.poi == HexPoiMark.PRESENT) Palette.ink.toArgb() else Color.WHITE
            canvas.drawPath(drop, dropFill)
            dropStroke.color = Palette.ink.toArgb()
            dropStroke.strokeWidth = 1.4f * density
            canvas.drawPath(drop, dropStroke)
        }
    }
}

private fun Int.withAlpha(fraction: Float): Int =
    (this and 0x00FFFFFF) or (((fraction * 255).toInt().coerceIn(0, 255)) shl 24)
