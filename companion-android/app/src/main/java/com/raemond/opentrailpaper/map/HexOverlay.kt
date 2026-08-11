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
 * Flat hexagons: the area being selected for download, and coverage we know
 * about but can't paint in the device's ink right now.
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
    class Hex(val outline: List<LatLon>, val style: Style, val check: Boolean = false)

    enum class Style {
        /** In the drawn box, queued to download. */
        SELECTION_PENDING,

        /** Built and sent this run. */
        SELECTION_DONE,

        /** Tapped out of the selection. */
        SELECTION_EXCLUDED,

        /** Downloaded, but its geometry isn't drawable right now. */
        OUTLINE_PHONE,

        /** As above, and the device has it too. */
        OUTLINE_SYNCED,

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
                OUTLINE_PHONE -> Palette.faint.toArgb().withAlpha(0.14f)
                OUTLINE_SYNCED -> Palette.good.toArgb().withAlpha(0.14f)
                MISSING -> Palette.accent.toArgb().withAlpha(0.10f)
            }

        val stroke: Int
            get() = when (this) {
                SELECTION_PENDING -> Palette.accent.toArgb()
                SELECTION_DONE -> Palette.good.toArgb()
                SELECTION_EXCLUDED -> Palette.muted.toArgb().withAlpha(0.55f)
                OUTLINE_PHONE -> Palette.faint.toArgb()
                OUTLINE_SYNCED -> Palette.good.toArgb()
                MISSING -> Palette.accent.toArgb().withAlpha(0.7f)
            }
    }

    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val strokePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 1.5f * density
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
            canvas.drawPath(path, fillPaint)
            canvas.drawPath(path, strokePaint)
            if (hex.check) {
                SyncedCheck.draw(canvas, cx / hex.outline.size, cy / hex.outline.size, density)
            }
        }
    }
}

/**
 * The badge that says "the device has this one too".
 *
 * Drawn rather than tinted from a vector asset: the mark has to hold its colour
 * over both paper and a dark water screentone, and the white ring is what keeps
 * it legible on the dark half. 14 dp across — it is a status badge on a ~5.6 km
 * hexagon, not a pin: any larger and neighbouring checks nearly touch when
 * zoomed out.
 */
object SyncedCheck {
    /** 14 dp across. */
    private const val D = 14f
    private val ringPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = Color.WHITE
    }
    private val discPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = Palette.good.toArgb()
    }
    private val tickPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        color = Color.WHITE
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }

    fun draw(canvas: Canvas, cx: Float, cy: Float, density: Float) {
        val d = D * density
        val k = d / 22f      // the marks below were drawn at 22
        val r = d / 2
        tickPaint.strokeWidth = 2.4f * k
        canvas.drawCircle(cx, cy, r, ringPaint)
        canvas.drawCircle(cx, cy, r - 1.5f * k, discPaint)
        val left = cx - r
        val top = cy - r
        val tick = Path().apply {
            moveTo(left + 6.2f * k, top + 11.4f * k)
            lineTo(left + 9.6f * k, top + 14.8f * k)
            lineTo(left + 15.8f * k, top + 7.4f * k)
        }
        canvas.drawPath(tick, tickPaint)
    }
}

private fun Int.withAlpha(fraction: Float): Int =
    (this and 0x00FFFFFF) or (((fraction * 255).toInt().coerceIn(0, 255)) shl 24)
