package com.raemond.opentrailpaper.ui

import android.provider.Settings
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.asAndroidPath
import androidx.compose.ui.graphics.asComposePath
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipPath
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.sin

// Tutorial icon art, drawn rather than placed: a single pen lays the outline down
// live, the panel's screentone develops behind it, and the whole badge flashes
// once as it settles — which is exactly the sequence the head unit goes through on
// every full refresh. It makes the first screens of the app feel like the device
// they're introducing.
//
// Drawn in a Canvas off a frame clock rather than with animation modifiers because
// the pen head has to sit at the END of the line being drawn, and that point only
// exists per-frame: an interpolated progress value would never reach it.

/**
 * What to draw. Each case is a set of strokes in a 100×100 space, listed in pen
 * order — the badge ring first, then the glyph.
 */
enum class SketchGlyph { LOCATION, WAVES, CHECK }

private fun strokes(glyph: SketchGlyph, side: Float): Path {
    val k = side / 100f
    fun x(v: Float) = v * k
    val path = Path()

    // The ring, clockwise from the top. First, so the badge draws itself before
    // whatever goes inside it.
    arc(path, Offset(x(50f), x(50f)), 46f * k, -90f, 360f)

    when (glyph) {
        SketchGlyph.LOCATION -> {
            // The location arrow, one closed outline.
            path.moveTo(x(75f), x(26f))
            path.lineTo(x(26f), x(48f))
            path.lineTo(x(47f), x(55f))
            path.lineTo(x(54f), x(75f))
            path.lineTo(x(75f), x(26f))
        }

        SketchGlyph.WAVES -> {
            // A transmitter: the dot, then each pair of waves spreading outward.
            path.addOval(Rect(Offset(x(44f), x(44f)), Size(x(12f), x(12f))))
            for (r in listOf(20f, 32f)) {
                arc(path, Offset(x(50f), x(50f)), r * k, 140f, 80f)
                arc(path, Offset(x(50f), x(50f)), r * k, -40f, 80f)
            }
        }

        SketchGlyph.CHECK -> {
            path.moveTo(x(30f), x(52f))
            path.lineTo(x(44f), x(67f))
            path.lineTo(x(71f), x(33f))
        }
    }
    return path
}

/**
 * `addArc` joins to the current point, which would trail a stray line in from
 * wherever the pen was. Always start the arc's own subpath.
 */
private fun arc(path: Path, center: Offset, radius: Float, startDeg: Float, sweepDeg: Float) {
    val start = Offset(
        center.x + radius * cos(startDeg * PI / 180).toFloat(),
        center.y + radius * sin(startDeg * PI / 180).toFloat(),
    )
    path.moveTo(start.x, start.y)
    path.arcTo(
        Rect(center.x - radius, center.y - radius, center.x + radius, center.y + radius),
        startDeg,
        sweepDeg,
        forceMoveTo = false,
    )
}

@Composable
fun SketchIcon(
    glyph: SketchGlyph,
    modifier: Modifier = Modifier,
    tint: Color = Palette.accent,
    side: androidx.compose.ui.unit.Dp = 132.dp,
    /** True while this is the page on screen. Flipping it replays the drawing, so
     *  swiping back and forth re-draws instead of showing a stale result. */
    active: Boolean = true,
) {
    val drawTime = 1.15f
    val flashTime = 0.24f
    val developTime = 0.5f
    val total = drawTime + flashTime + developTime

    // "Remove animations" (animator duration scale 0) gets the settled state with
    // no drawing at all — the Android equivalent of iOS's Reduce Motion, and the
    // one accessibility setting this art has to honour.
    val context = LocalContext.current
    val reduceMotion = remember {
        Settings.Global.getFloat(
            context.contentResolver,
            Settings.Global.ANIMATOR_DURATION_SCALE,
            1f,
        ) == 0f
    }
    var elapsed by remember { mutableFloatStateOf(total) }

    LaunchedEffect(glyph, active) {
        if (!active || reduceMotion) {
            elapsed = total
            return@LaunchedEffect
        }
        elapsed = 0f
        val start = withFrameNanos { it }
        // Stops as soon as it's done — otherwise it keeps ticking at 60 Hz behind
        // a finished drawing for as long as the tutorial is open.
        while (elapsed < total) {
            val now = withFrameNanos { it }
            elapsed = ((now - start) / 1_000_000_000.0).toFloat()
        }
        elapsed = total
    }

    Canvas(modifier.size(side)) {
        render(elapsed, tint, drawTime, flashTime, developTime, glyph)
    }
}

private fun DrawScope.render(
    elapsed: Float,
    tint: Color,
    drawTime: Float,
    flashTime: Float,
    developTime: Float,
    glyph: SketchGlyph,
) {
    val s = minOf(size.width, size.height)
    if (s <= 0f) return
    val k = s / 100f

    val pen = (elapsed / drawTime).coerceIn(0f, 1f)
    val after = elapsed - drawTime
    // One up-and-back pulse, like the panel's settle flash.
    val flash = if (after in 0f..flashTime) sin(PI * after / flashTime).toFloat() else 0f
    val develop = ((after - flashTime) / developTime).coerceIn(0f, 1f)

    val disc = Path().apply {
        addOval(Rect(2f * k, 2f * k, s - 2f * k, s - 2f * k))
    }
    drawPath(disc, Palette.surface)

    // The screentone the map uses, in miniature: a dot screen rather than a flat
    // tint, developing after the ink is down.
    if (develop > 0f) {
        clipPath(disc) {
            val pitch = 5f * k
            val r = 1.15f * k
            var y = pitch
            while (y < s) {
                var x = pitch
                while (x < s) {
                    drawCircle(tint.copy(alpha = 0.5f * develop), r, Offset(x, y))
                    x += pitch
                }
                y += pitch
            }
        }
    }

    val full = strokes(glyph, s)
    val style = Stroke(width = 3.4f * k, cap = StrokeCap.Round, join = StrokeJoin.Round)
    val (drawn, head) = trim(full, pen)
    drawPath(drawn, tint, style = style)

    // The pen itself, riding the end of the line.
    if (pen > 0f && pen < 1f && head != null) {
        drawCircle(tint, 2.6f * k, head)
    }

    // The settle flash: the badge inverts for an instant and clears.
    if (flash > 0f) {
        drawPath(disc, Palette.ink.copy(alpha = flash * 0.9f))
        drawPath(full, Palette.paper.copy(alpha = flash * 0.9f), style = style)
    }
}

/**
 * The first [fraction] of a path by arc length, across all its subpaths, plus the
 * point the pen has reached. Compose's PathMeasure walks one contour at a time, so
 * the total is accumulated first and then spent contour by contour.
 */
private fun trim(path: Path, fraction: Float): Pair<Path, Offset?> {
    if (fraction >= 1f) return path to null
    val out = android.graphics.Path()
    if (fraction <= 0f) return out.asComposePath() to null

    // android.graphics.PathMeasure rather than Compose's: only the platform one
    // walks from one contour to the next, and these glyphs are several separate
    // strokes that have to be drawn in pen order.
    val measure = android.graphics.PathMeasure(path.asAndroidPath(), false)
    val lengths = ArrayList<Float>()
    var total = 0f
    do {
        val len = measure.length
        lengths.add(len)
        total += len
    } while (measure.nextContour())
    if (total <= 0f) return out.asComposePath() to null

    var budget = total * fraction
    var head: Offset? = null
    val cursor = android.graphics.PathMeasure(path.asAndroidPath(), false)
    val pos = FloatArray(2)
    var index = 0
    do {
        if (budget <= 0f) break
        val take = minOf(budget, lengths.getOrElse(index) { 0f })
        if (take > 0f) {
            cursor.getSegment(0f, take, out, true)
            if (cursor.getPosTan(take, pos, null)) head = Offset(pos[0], pos[1])
        }
        budget -= take
        index += 1
    } while (cursor.nextContour())
    return out.asComposePath() to head
}
