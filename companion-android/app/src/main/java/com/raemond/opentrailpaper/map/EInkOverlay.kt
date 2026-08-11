package com.raemond.opentrailpaper.map

import android.graphics.Bitmap
import android.graphics.BitmapShader
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.Path
import android.graphics.Point
import android.graphics.Shader
import androidx.compose.ui.graphics.toArgb
import com.raemond.opentrailpaper.ui.Palette
import org.osmdroid.util.GeoPoint
import org.osmdroid.views.MapView
import org.osmdroid.views.Projection
import org.osmdroid.views.overlay.Overlay
import kotlin.math.cos
import kotlin.math.log2
import kotlin.math.pow
import kotlin.math.roundToInt

/**
 * Paints one downloaded area the way the head unit will render it: black ink on
 * paper, roads by class, water as a dot screen and parks as a diagonal hatch —
 * the screentones in src/map_view.cpp — clipped exactly to its H3 hexagon so
 * neighbouring areas meet seamlessly.
 *
 * osmdroid rather than a Compose Canvas layered over the map, for the same
 * reason iOS uses MKOverlayRenderer rather than a SwiftUI Canvas: only an
 * overlay drawn inside the map's own draw pass stays pinned to the ground
 * frame-for-frame. Anything composited above redraws a frame late and visibly
 * swims while panning.
 */
class EInkOverlay(
    private val area: EInkArea,
    /**
     * Screen pixels per density-independent pixel. Every width and shedding
     * threshold below is quoted in iOS POINTS, which are dp here — a Canvas works
     * in raw pixels, so without this a road is drawn ~3x too thin on a modern
     * phone and the map keeps detail the device would have shed.
     */
    private val density: Float,
) : Overlay() {

    private val hexPath = Path()          // in world units relative to the tile origin
    private val screenHex = Path()
    private val matrix = Matrix()
    private val probe = Point()
    private val probe2 = Point()
    private val hexBounds = android.graphics.RectF()

    private val paperPaint = Paint().apply {
        style = Paint.Style.FILL
        color = Palette.paper.toArgb()
    }
    private val inkPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
        color = Color.BLACK
    }
    private val tonePaint = Paint().apply {
        style = Paint.Style.FILL
        isFilterBitmap = false            // keep the tone hard-edged, not blurred
    }
    private val edgePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
    }

    init {
        val tile = area.tile
        for ((i, c) in area.hexagon.withIndex()) {
            val x = (MercatorWorld.x(c.lon) - tile.originX).toFloat()
            val y = (MercatorWorld.y(c.lat) - tile.originY).toFloat()
            if (i == 0) hexPath.moveTo(x, y) else hexPath.lineTo(x, y)
        }
        hexPath.close()
    }

    override fun draw(canvas: Canvas, projection: Projection) {
        val tile = area.tile

        // World units -> screen. The map is never rotated here, so the transform
        // is a translate and a uniform scale, and two probes describe it exactly.
        // The second probe is deliberately a long way off (1/64 of the world in
        // Mercator Y): screen coordinates come back as integers, so a short
        // baseline would quantise the scale badly at the zooms where the ink is
        // actually legible.
        projection.toPixels(GeoPoint(tile.originLat, tile.originLon), probe)
        val probeWorldY = tile.originY + PROBE_SPAN
        val probeLat = MercatorWorld.latFromY(probeWorldY)
        projection.toPixels(GeoPoint(probeLat, tile.originLon), probe2)
        val screenSpan = (probe2.y - probe.y).toDouble()
        if (screenSpan <= 0) return
        val scale = screenSpan / PROBE_SPAN
        if (!scale.isFinite() || scale <= 0) return

        matrix.setTranslate(probe.x.toFloat(), probe.y.toFloat())
        matrix.preScale(scale.toFloat(), scale.toFloat())

        // The edge is drawn last and unclipped, so keep a screen-space copy.
        hexPath.transform(matrix, screenHex)

        canvas.save()
        canvas.concat(matrix)
        canvas.clipPath(hexPath)

        // Paper first. Bounded by the hexagon's own bounds rather than the whole
        // canvas, since the clip already stops at the hex edge.
        // The one-argument overload is API 34; this app runs from 26, so the
        // deprecated-in-34 form is the only one that exists everywhere.
        @Suppress("DEPRECATION")
        hexPath.computeBounds(hexBounds, true)
        canvas.drawRect(hexBounds, paperPaint)

        // The device sheds detail by metres-per-pixel; mirroring that keeps the
        // preview honest AND bounds the work when zoomed out.
        val mapSizePx = scale * MercatorWorld.WORLD
        val metersPerPoint =
            EQUATOR_M * cos(area.center.lat * Math.PI / 180) / mapSizePx * density

        // Parks under water: a lake inside a park is water, not parkland.
        tile.parks?.let { screentone(canvas, it, hatchTone, HATCH_CELL_DP * density, scale) }
        tile.water?.let { screentone(canvas, it, dotTone, DOT_CELL_DP * density, scale) }

        for (cls in DRAW_ORDER) {
            val width = widthFor(cls, metersPerPoint) ?: continue
            val path = tile.roads[cls] ?: continue
            inkPaint.strokeWidth = (width * density / scale).toFloat()
            // A path/trail is a dithered grey line on the panel; a fine dash is
            // the closest thing that still reads as 1-bit here.
            inkPaint.pathEffect = if (cls == Ebm.FeatureClass.PATH) {
                val d = (1.5 * density / scale).toFloat()
                android.graphics.DashPathEffect(floatArrayOf(d, d), 0f)
            } else {
                null
            }
            canvas.drawPath(path, inkPaint)
        }
        canvas.restore()

        // Edge last and unclipped, so the full stroke shows: green where the
        // device has this area too, quiet hairline where it's only on the phone.
        edgePaint.color = (if (area.synced) Palette.good else Palette.faint).toArgb()
        edgePaint.strokeWidth = (if (area.synced) 2f else 1f) * density
        canvas.drawPath(screenHex, edgePaint)
    }

    /**
     * Fills [path] with a 1-bit tone, drawn at a constant size on screen.
     *
     * The tone is anchored to a GLOBAL grid in world space, with the cell
     * quantised to a power of two. Both matter: each downloaded hex is a separate
     * overlay, so anchoring the pattern to anything local would print a visible
     * seam at every hexagon boundary, and a cell that isn't a power of two drifts
     * out of phase between neighbours as the zoom changes.
     *
     * The shader's local matrix maps tone space into the canvas's CURRENT space,
     * which is world-relative here — so the tone is placed in world units and
     * inherits the canvas scale, and no path has to be transformed per frame.
     */
    private fun screentone(
        canvas: Canvas,
        path: Path,
        tone: Bitmap?,
        cellPx: Double,
        scale: Double,
    ) {
        if (tone == null) return
        val rawCellWorld = cellPx / scale
        if (rawCellWorld <= 0 || !rawCellWorld.isFinite()) return
        val cellWorld = 2.0.pow(log2(rawCellWorld).roundToInt())
        if (cellWorld * scale < 0.01) return

        // Global grid line nearest the tile origin, expressed in canvas-local
        // (origin-relative) coordinates.
        val originX = area.tile.originX
        val originY = area.tile.originY
        val anchorX = (kotlin.math.floor(originX / cellWorld) * cellWorld - originX).toFloat()
        val anchorY = (kotlin.math.floor(originY / cellWorld) * cellWorld - originY).toFloat()

        val shader = BitmapShader(tone, Shader.TileMode.REPEAT, Shader.TileMode.REPEAT)
        val local = Matrix()
        local.setTranslate(anchorX, anchorY)
        local.preScale(
            (cellWorld / tone.width).toFloat(),
            (cellWorld / tone.height).toFloat(),
        )
        shader.setLocalMatrix(local)
        tonePaint.shader = shader
        canvas.drawPath(path, tonePaint)
        tonePaint.shader = null
    }

    override fun onDetach(mapView: MapView?) {
        tonePaint.shader = null
        super.onDetach(mapView)
    }

    companion object {
        /** Metres around the equator — the scale-bar constant behind metres/pixel. */
        private const val EQUATOR_M = 40_075_016.686

        /** Probe baseline in world units: far enough that integer screen pixels
         *  quantise the derived scale to a few parts per million. */
        private const val PROBE_SPAN = MercatorWorld.WORLD / 64.0

        private const val DOT_CELL_DP = 3.0
        private const val HATCH_CELL_DP = 6.0

        /** Thin classes first so a motorway crosses on top of a footpath, as on
         *  the device (it draws in the same order). */
        private val DRAW_ORDER = listOf(
            Ebm.FeatureClass.PATH,
            Ebm.FeatureClass.MINOR,
            Ebm.FeatureClass.TERTIARY,
            Ebm.FeatureClass.SECONDARY,
            Ebm.FeatureClass.PRIMARY,
            Ebm.FeatureClass.ARTERIAL,
        )

        /**
         * Stroke width in screen pixels, or null when the device would shed this
         * class at this zoom (map_view.cpp styleFor + map_tiles.cpp shedding).
         */
        private fun widthFor(cls: Ebm.FeatureClass, mpp: Double): Double? = when (cls) {
            Ebm.FeatureClass.PATH -> if (mpp >= 4) null else 1.0
            Ebm.FeatureClass.MINOR -> if (mpp >= 16) null else 2.0
            Ebm.FeatureClass.SECONDARY -> if (mpp >= 32) null else 3.0
            Ebm.FeatureClass.TERTIARY -> if (mpp >= 32) null else 2.0
            Ebm.FeatureClass.PRIMARY -> if (mpp >= 16) 2.0 else 4.0
            Ebm.FeatureClass.ARTERIAL -> if (mpp >= 16) 2.0 else 5.0
        }

        // The device's tones are dots for water and diagonal stripes for parks,
        // and that CHARACTER is what has to survive here — it's how the two read
        // apart at a glance. Their exact ink coverage cannot: on a 235 ppi panel
        // the water's 75% dots integrate into a dark grey, but a phone screentone
        // big enough to still look dotted is coarse enough that 75% is simply
        // black, and the bay swallows the map. So the patterns are kept and the
        // coverage is lightened, preserving the device's ordering — water darker
        // than parkland.
        private val dotTone: Bitmap? by lazy { makeTone(2) { x, y -> (x and 1) == (y and 1) } }
        private val hatchTone: Bitmap? by lazy { makeTone(4) { x, y -> ((x - y) and 3) < 1 } }

        /** A tiny opaque-black-on-clear bitmap, tiled by [screentone]. */
        private fun makeTone(size: Int, on: (Int, Int) -> Boolean): Bitmap? {
            val px = IntArray(size * size)
            for (y in 0 until size) {
                for (x in 0 until size) {
                    px[y * size + x] = if (on(x, y)) Color.BLACK else Color.TRANSPARENT
                }
            }
            return Bitmap.createBitmap(px, size, size, Bitmap.Config.ARGB_8888)
        }
    }
}
