package com.raemond.opentrailpaper.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.graphics.drawscope.rotate as rotateScope
import androidx.compose.ui.graphics.drawscope.scale
import androidx.compose.ui.graphics.drawscope.translate
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.data.DashLayout
import com.raemond.opentrailpaper.data.RideSim
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.floor
import kotlin.math.sqrt

// A working head unit, drawn on the phone.
//
// The tutorial used to introduce the device with two flat product shots. A
// photograph of a bike computer is indistinguishable from a photograph of a brick:
// nothing about it says the numbers are live, that the map moves under you, or
// that the panel is yours to rearrange. So the welcome screen runs the device
// instead of picturing it — same body, same panel geometry, same fields, ticking
// off RideSim.
//
// The dashboard face is the editor's DashPreview with a ride plugged into it, not
// a second drawing of the same thing. There is exactly one renderer of the
// device's panel in this app and this is a client of it, which is what stops the
// tutorial's idea of the device drifting from the one the rider configures.

enum class DeviceFace {
    DASHBOARD,   // the numbers
    MAP;         // riding a route

    /** The screen the OTHER device in a pair should show, so a hero always
     *  presents two different faces rather than the same one twice. */
    val counterpart: DeviceFace get() = if (this == DASHBOARD) MAP else DASHBOARD
}

private const val PANEL_WIDTH = 540f
private const val PANEL_HEIGHT = 960f
private const val PANEL_STATUS_H = 64f

/**
 * The head unit: body, screen, status bar, and whichever face is showing.
 * [width] is the SCREEN width; the body sizes itself around it in the same
 * proportions as the product renders.
 *
 * @param live frozen at a fixed instant unless told to run. A still device is the
 * right thing behind a live one — two panels both counting draws the eye to the
 * wrong place, and the back of the pair is mostly hidden anyway.
 */
@Composable
fun DeviceMock(
    face: DeviceFace,
    width: Dp,
    modifier: Modifier = Modifier,
    live: Boolean = true,
) {
    val k = width.value / PANEL_WIDTH
    val screenH = (PANEL_HEIGHT * k).dp
    // Body proportions, measured off the renders: the case is a little wider than
    // the glass, with a tall chin for the button.
    val bodyW = width * 1.115f
    val bodyH = screenH + width * 0.466f

    var t by remember { mutableFloatStateOf(0f) }
    LaunchedEffect(live) {
        if (!live) { t = 0f; return@LaunchedEffect }
        val start = withFrameNanos { it }
        while (true) {
            val now = withFrameNanos { it }
            t = ((now - start) / 1_000_000_000.0).toFloat()
        }
    }

    // The panel's numbers step once a second because that is how often the e-paper
    // redraws them — a value smoothly interpolating would be the one part of this
    // the real device cannot do. The map is the deliberate exception: it pans on
    // every frame, because a map stepping at 1 Hz reads as a bug rather than as ink.
    val smooth = RideSim(t.toDouble())
    val stepped = RideSim(floor(t).toDouble())

    Box(modifier.size(bodyW, bodyH), contentAlignment = Alignment.TopCenter) {
        DeviceBody(bodyW, bodyH)
        Column(
            Modifier
                .padding(top = width * 0.221f)
                .size(width, screenH)
                .background(Palette.paper)
                .border((2 * k).dp, Color.Black)
                .clipToBounds(),
        ) {
            val density = LocalDensity.current
            CompositionLocalProvider(
                LocalDensity provides Density(density.density, fontScale = 1f),
            ) {
                StatusBar(stepped, k, Modifier.height((PANEL_STATUS_H * k).dp))
                Box(Modifier.fillMaxWidth().height((2 * k).dp).background(Color.Black))
                when (face) {
                    DeviceFace.DASHBOARD -> DashPreview(
                        DashLayout.tutorial,
                        Modifier.fillMaxSize(),
                        live = stepped,
                    )

                    DeviceFace.MAP -> MapFace(smooth, stepped, k, Modifier.fillMaxSize())
                }
            }
        }
    }
}

/**
 * A [DeviceMock] that changes screens the way the device does: e-paper cannot
 * cross-fade, it drives every pixel to black and back, so switching faces here
 * flashes once and the new screen is simply THERE afterwards. A dissolve would be
 * the prettier lie; this is the thing a rider will recognise the first time they
 * press the button on the real unit.
 */
@Composable
fun EInkDevice(
    face: DeviceFace,
    width: Dp,
    modifier: Modifier = Modifier,
    live: Boolean = true,
) {
    var shown by remember { mutableStateOf(face) }
    var flash by remember { mutableFloatStateOf(0f) }

    LaunchedEffect(face) {
        if (face == shown) return@LaunchedEffect
        val start = withFrameNanos { it }
        // Down over ~90 ms, hold, then back up over ~200 ms.
        while (true) {
            val now = withFrameNanos { it }
            val ms = (now - start) / 1_000_000.0
            flash = when {
                ms < 90 -> (ms / 90).toFloat()
                ms < 240 -> 1f
                ms < 440 -> (1 - (ms - 240) / 200).toFloat()
                else -> 0f
            }
            if (ms >= 150 && shown != face) shown = face
            if (ms >= 440) { flash = 0f; break }
        }
    }

    Box(modifier) {
        DeviceMock(shown, width, live = live)
        if (flash > 0f) {
            // Only the glass flashes, not the case — the panel is the part that
            // refreshes. The glass sits a hair above the body's centre, because
            // the chin under the screen is taller than the brow above it.
            Box(
                Modifier
                    .align(Alignment.Center)
                    .offset(y = -width * 0.012f)
                    .size(width, width / PANEL_WIDTH * PANEL_HEIGHT)
                    .background(Color.Black.copy(alpha = flash)),
            )
        }
    }
}

/** The case: warm plastic, a soft edge, the sleep button on the left rail and the
 *  round confirm button under the glass. */
@Composable
private fun DeviceBody(bodyW: Dp, bodyH: Dp) {
    Box(Modifier.size(bodyW, bodyH)) {
        Box(
            Modifier
                .fillMaxSize()
                .background(Color(0xFFF4F1E8), RoundedCornerShape(bodyW * 0.092f))
                .border(1.dp, Color(0xFFD8D3C4), RoundedCornerShape(bodyW * 0.092f)),
        )
        Box(
            Modifier
                .align(Alignment.TopStart)
                .offset(x = -bodyW * 0.011f, y = bodyH * 0.52f)
                .size(bodyW * 0.022f, bodyH * 0.075f)
                .background(Color(0xFFE6E1D2), RoundedCornerShape(bodyW * 0.02f)),
        )
        Box(
            Modifier
                .align(Alignment.BottomCenter)
                .padding(bottom = bodyH * 0.02f)
                .size(bodyW * 0.157f)
                .border(bodyW * 0.008f, Color(0xFFC9C3B2), CircleShape),
            contentAlignment = Alignment.Center,
        ) {
            Box(Modifier.size(bodyW * 0.028f).background(Color(0xFFB8B2A0), CircleShape))
        }
    }
}

/**
 * The panel's top strip: clock, phone link, satellites, which sensors are paired,
 * and the fuel gauge — the same order and the same abbreviations the device
 * prints, because this is the one row a rider learns to read at a glance and
 * reordering it here would teach the wrong glance.
 */
@Composable
private fun StatusBar(sim: RideSim, k: Float, modifier: Modifier = Modifier) {
    Row(
        modifier
            .fillMaxWidth()
            .padding(horizontal = (18 * k).dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy((9 * k).dp),
    ) {
        Text(
            RideSim.hm(sim.clock),
            style = condensed((40 * k).sp, FontWeight.Bold),
            color = Color.Black,
            maxLines = 1,
        )
        // The phone-link glyph.
        Box(
            Modifier
                .size((15 * k).dp, (26 * k).dp)
                .border((3 * k).dp, Color.Black, RoundedCornerShape((3 * k).dp)),
        )
        // One dot per satellite, capped at four — the device shows lock quality,
        // not an exact count, and four dots is what "plenty" looks like. Empty
        // dots stay as outlines: dropping them would let the row change width
        // every time the sky opened up, and a status bar that reflows is one you
        // stop trusting to be in the same place.
        Row(horizontalArrangement = Arrangement.spacedBy((4 * k).dp)) {
            val lit = (sim.satellites - 8).coerceIn(0, 4)
            repeat(4) { i ->
                Box(
                    Modifier
                        .size((9 * k).dp)
                        .background(if (i < lit) Color.Black else Color.Transparent, CircleShape)
                        .border((1.5f * k).dp, Color.Black, CircleShape),
                )
            }
        }
        Text(
            "· HR · PWR",
            style = condensed((36 * k).sp, FontWeight.Bold),
            color = Color.Black,
            maxLines = 1,
        )
        Spacer(Modifier.weight(1f))
        Text(
            "${sim.battery}%",
            style = condensed((36 * k).sp, FontWeight.Bold),
            color = Color.Black,
            maxLines = 1,
        )
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(
                Modifier
                    .size((44 * k).dp, (22 * k).dp)
                    .border((3 * k).dp, Color.Black, RoundedCornerShape((2 * k).dp))
                    .padding((3.5f * k).dp),
            ) {
                Box(
                    Modifier
                        .fillMaxHeight()
                        .width((44 * k * sim.battery / 100f).dp)
                        .background(Color.Black, RoundedCornerShape((1 * k).dp)),
                )
            }
            Spacer(Modifier.width((1.5f * k).dp))
            Box(
                Modifier
                    .size((4 * k).dp, (10 * k).dp)
                    .background(Color.Black, RoundedCornerShape((1 * k).dp)),
            )
        }
    }
}

// MARK: - the map face

/**
 * The map the device draws while you ride: ink on paper, roads by class, parks as
 * a diagonal hatch — the screentones of src/map_view.cpp, at the size they survive
 * being shrunk to a hero.
 *
 * It scrolls off the ride's DISTANCE rather than a free-running animation, so the
 * map and the numbers next to it are the same ride: when the speed dips, the
 * ground under the arrow slows down with it.
 */
@Composable
private fun MapFace(smooth: RideSim, stepped: RideSim, k: Float, modifier: Modifier = Modifier) {
    // The map is laid out in the same points the panel is, but a DrawScope
    // measures in pixels — so the street grid is scaled by the display density
    // or it comes out far too fine to read as a city.
    val density = LocalDensity.current.density
    Column(modifier.background(PanelPaper)) {
        Box(Modifier.fillMaxWidth().weight(1f)) {
            val s = (smooth.distance * 1000 / METRES_PER_POINT).toFloat()
            Canvas(Modifier.fillMaxSize()) {
                drawCity(s, density)
                drawRider(s, k * density)
                drawChrome(k * density, density)
            }
            // The scale bar's caption. Text on a Canvas needs a measurer and a
            // typeface handle; a Text over it uses the same Barlow the rest of
            // the panel is set in, which is what makes it look like the device.
            Text(
                "200 M",
                style = condensed((34 * k).sp, FontWeight.Bold),
                color = Color.Black,
                modifier = Modifier
                    .align(Alignment.BottomStart)
                    .padding(start = (22 * k).dp, bottom = (30 * k).dp),
            )
        }
        // The strip the map face keeps below it, so the numbers never disappear
        // just because you are looking at where you are.
        Row(
            Modifier
                .fillMaxWidth()
                .height((150 * k).dp)
                .background(PanelPaper),
        ) {
            StripCell("SPEED", stepped.text("speed"), k, Modifier.weight(1f))
            StripCell("DIST", stepped.text("distance"), k, Modifier.weight(1f))
            StripCell("TIME", RideSim.hm(stepped.elapsed), k, Modifier.weight(1f))
        }
    }
}

@Composable
private fun StripCell(label: String, value: String, k: Float, modifier: Modifier = Modifier) {
    Column(
        modifier
            .fillMaxHeight()
            .padding(vertical = (8 * k).dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text(
            label,
            style = barlow((26 * k).sp, FontWeight.SemiBold)
                .copy(letterSpacing = (2 * k).sp),
            color = Color.Black,
        )
        Text(
            value,
            style = condensed((76 * k).sp, FontWeight.Bold),
            color = Color.Black,
            maxLines = 1,
        )
    }
}

/** Metres of ground per point, so the scale bar below is not a decoration. */
private const val METRES_PER_POINT = 2.6f
private const val GRID = 46f            // block size, points
private const val GRID_ANGLE = -21f     // the street grid is never square on

// The route, as legs through the block grid: five blocks up, two across, three up,
// two back. Each leg runs ALONG a street rather than across the middle of a block,
// and the pattern repeats, so an unbounded ride needs no stored polyline — just
// the leg you are on and how many times round.
private val LEGS = listOf(
    Offset(0f, -5 * GRID),
    Offset(2 * GRID, 0f),
    Offset(0f, -3 * GRID),
    Offset(-2 * GRID, 0f),
)
private const val LAP_LENGTH = 12 * GRID
private val LAP_SHIFT = Offset(0f, -8 * GRID)

/** Where the rider is after [s] points of road. */
private fun pointAt(s: Float): Offset {
    val lap = floor(s / LAP_LENGTH)
    var left = s - lap * LAP_LENGTH
    var p = Offset(LAP_SHIFT.x * lap, LAP_SHIFT.y * lap)
    for (leg in LEGS) {
        val len = abs(leg.x) + abs(leg.y)
        if (left <= len) {
            val f = if (len == 0f) 0f else left / len
            return Offset(p.x + leg.x * f, p.y + leg.y * f)
        }
        p = Offset(p.x + leg.x, p.y + leg.y)
        left -= len
    }
    return p
}

/**
 * Which way the rider is pointing, measured as the chord across a few metres
 * either side. Taking the leg's own direction would snap the arrow through ninety
 * degrees in one frame at every corner; the chord rounds the turn over the second
 * or so it takes to make it.
 */
private fun headingAt(s: Float): Float {
    val a = pointAt(maxOf(0f, s - 9f))
    val b = pointAt(s + 9f)
    // +90° because the arrow art points up, not along +x.
    return Math.toDegrees(atan2((b.y - a.y).toDouble(), (b.x - a.x).toDouble())).toFloat() + 90f
}

/** Draws the city with the rider's own position under the centre of the screen. */
private fun DrawScope.drawCity(s: Float, density: Float) {
    drawRect(PanelPaper)
    val me = pointAt(s)
    // Points -> pixels, once, so every constant below stays in panel points.
    scale(density, pivot = Offset.Zero) {
    val w = size.width / density
    val h = size.height / density
    translate(w / 2, h / 2) {
        rotateScope(GRID_ANGLE, Offset.Zero) {
            translate(-me.x, -me.y) {
                // Everything within the screen's half-diagonal of the rider can
                // end up on screen once it is rotated, so that is what gets drawn.
                val reach = 0.5f * sqrt(w * w + h * h) + GRID
                val c0 = floor((me.x - reach) / GRID).toInt()
                val c1 = kotlin.math.ceil((me.x + reach) / GRID).toInt()
                val r0 = floor((me.y - reach) / GRID).toInt()
                val r1 = kotlin.math.ceil((me.y + reach) / GRID).toInt()

                // Blocks first, so every line lands on top of its own edge.
                for (c in c0 until c1) {
                    for (r in r0 until r1) {
                        val tone = toneAt(c, r) ?: continue
                        val rect = Rect(
                            c * GRID + 2, r * GRID + 2,
                            (c + 1) * GRID - 2, (r + 1) * GRID - 2,
                        )
                        hatch(rect, dense = tone == 1)
                    }
                }

                val minor = Path()
                for (c in c0..c1) {
                    minor.moveTo(c * GRID, r0 * GRID)
                    minor.lineTo(c * GRID, r1 * GRID)
                }
                for (r in r0..r1) {
                    minor.moveTo(c0 * GRID, r * GRID)
                    minor.lineTo(c1 * GRID, r * GRID)
                }
                drawPath(minor, Color.Black, style = Stroke(width = 1f))

                // Arterials, drawn heavier — a grid of identical lines reads as
                // graph paper, and the device's map is legible precisely because
                // road class comes through as line weight.
                val major = Path()
                for (c in c0..c1) {
                    if (c % 5 != 0) continue
                    major.moveTo(c * GRID, r0 * GRID)
                    major.lineTo(c * GRID, r1 * GRID)
                }
                for (r in r0..r1) {
                    if (r % 4 != 0) continue
                    major.moveTo(c0 * GRID, r * GRID)
                    major.lineTo(c1 * GRID, r * GRID)
                }
                drawPath(major, Color.Black, style = Stroke(width = 3.5f))

                // The loaded route, at the weight the device reserves for it.
                // Drawn a lap either side of the current one so it runs off both
                // edges of the screen rather than beginning under the rider.
                val lap = floor(s / LAP_LENGTH).toInt()
                val route = Path()
                for (n in (lap - 1)..(lap + 2)) {
                    var p = Offset(LAP_SHIFT.x * n, LAP_SHIFT.y * n)
                    route.moveTo(p.x, p.y)
                    for (leg in LEGS) {
                        p = Offset(p.x + leg.x, p.y + leg.y)
                        route.lineTo(p.x, p.y)
                    }
                }
                drawPath(
                    route,
                    Color.Black,
                    style = Stroke(width = 7f, cap = StrokeCap.Round, join = StrokeJoin.Round),
                )
            }
        }
    }
    }
}

/**
 * Which blocks are parks (0) or water (1). A hash rather than a stored map: the
 * grid scrolls forever and the pattern has to exist wherever it gets to.
 */
private fun toneAt(col: Int, row: Int): Int? {
    var h = (col * 73_856_093 xor row * 19_349_663).toUInt()
    h = h xor (h shr 13)
    h *= 2_654_435_761u
    h = h xor (h shr 16)
    return when (h % 23u) {
        0u, 1u -> 0     // park
        2u -> 1         // water
        else -> null
    }
}

/** The screentones, drawn as line work: parks hatch at 45°, water gets the tighter
 *  screen that reads as grey from arm's length. */
private fun DrawScope.hatch(rect: Rect, dense: Boolean) {
    clipRect(rect.left, rect.top, rect.right, rect.bottom) {
        val spacing = if (dense) 3f else 6f
        val p = Path()
        var x = rect.left - rect.height
        while (x < rect.right) {
            p.moveTo(x, rect.bottom)
            p.lineTo(x + rect.height, rect.top)
            x += spacing
        }
        drawPath(p, Color.Black, style = Stroke(width = if (dense) 1.4f else 1f))
    }
}

/**
 * The rider: fixed at the centre of the screen, turning to face the way the route
 * goes. The badge stays upright — only the arrow inside it swings — because the
 * ring is chrome and chrome does not rotate.
 */
private fun DrawScope.drawRider(s: Float, k: Float) {
    val centre = Offset(size.width / 2, size.height / 2)
    val r = 38 * k
    drawCircle(PanelPaper, r, centre)
    drawCircle(Color.Black, r, centre, style = Stroke(width = 2.5f * k))
    rotateScope(headingAt(s) + GRID_ANGLE, centre) {
        val a = 20 * k
        val arrow = Path().apply {
            moveTo(centre.x, centre.y - a)
            lineTo(centre.x + 0.7f * a, centre.y + 0.6f * a)
            lineTo(centre.x, centre.y + 0.15f * a)
            lineTo(centre.x - 0.7f * a, centre.y + 0.6f * a)
            close()
        }
        drawPath(arrow, Color.Black)
    }
}

private fun DrawScope.drawChrome(k: Float, density: Float) {
    // North arrow, top right.
    val nc = Offset(size.width - 42 * k, 42 * k)
    drawCircle(PanelPaper, 29 * k, nc)
    drawCircle(Color.Black, 29 * k, nc, style = Stroke(width = 2.5f * k))
    val n = Path().apply {
        moveTo(nc.x, nc.y - 11 * k)
        lineTo(nc.x + 10 * k, nc.y + 9 * k)
        lineTo(nc.x - 10 * k, nc.y + 9 * k)
        close()
    }
    drawPath(n, Color.Black)

    // The scale bar, and it means it: 200 m of real ground.
    val barW = 200 / METRES_PER_POINT * density
    val y = size.height - 24 * k
    drawRect(
        Color.Black,
        topLeft = Offset(22 * k, y),
        size = Size(barW, 3 * k),
    )
}
