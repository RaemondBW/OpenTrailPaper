package com.raemond.opentrailpaper.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Map
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Speed
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material.icons.automirrored.filled.ViewList
import androidx.compose.material3.Icon
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
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
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
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.roundToInt
import kotlin.math.sin
import kotlin.math.sqrt

// The phone, running this app — for the tutorial screen that explains what the
// APP is for.
//
// That screen used to show the head unit, which quietly said the wrong thing:
// planning a route, baking tiles and reading rides back are all things you do
// here, on the phone in your hand, and illustrating them with the bike computer
// put the work on the wrong device. So the three jobs are demonstrated on a phone,
// and the head unit stays on the welcome screen where it belongs.
//
// Drawn rather than screenshotted, for the same reason SketchIcon is drawn: a
// screenshot goes stale the first time a colour or a corner radius changes, while
// these are built from the same Palette and type scale as the real screens, so
// they age with them. It also lets them move — the route draws itself in, the
// tiles land one by one, and today's ride is still counting.

enum class AppFace {
    ROUTE,   // plan a route and send it over
    MAPS,    // bake offline tiles onto the card
    RIDES,   // read recorded rides back
}

@Composable
fun PhoneMock(
    face: AppFace,
    width: Dp,
    modifier: Modifier = Modifier,
    live: Boolean = true,
) {
    /** iPhone-class points → this mock's points, so the screens below can be
     *  written at the sizes they really are. */
    val u = width.value / 393f
    val screenH = width * 852f / 393f
    val bezel = width * 0.042f

    var faceElapsed by remember { mutableFloatStateOf(4f) }
    var shown by remember { mutableStateOf<AppFace?>(null) }
    var blank by remember { mutableFloatStateOf(0f) }

    // Screens are swapped through blank rather than cross-faded. Dissolving one
    // dense screen into another leaves two sets of headings and numbers legible on
    // top of each other for the length of the animation, which reads as a
    // rendering fault rather than as navigation.
    LaunchedEffect(face, live) {
        if (shown == null) { shown = face; faceElapsed = if (live) 0f else 4f }
        if (!live) return@LaunchedEffect
        if (shown != face) {
            val start = withFrameNanos { it }
            while (true) {
                val now = withFrameNanos { it }
                val ms = (now - start) / 1_000_000.0
                blank = when {
                    ms < 120 -> (ms / 120).toFloat()
                    ms < 250 -> 1f
                    ms < 410 -> (1 - (ms - 250) / 160).toFloat()
                    else -> 0f
                }
                if (ms >= 130 && shown != face) { shown = face; faceElapsed = 0f }
                if (ms >= 410) { blank = 0f; break }
            }
        }
        // Anything that plays out once is timed from when its screen came up, not
        // from when the tutorial opened — otherwise arriving here late shows a
        // progress bar that finished before it was seen.
        val start = withFrameNanos { it }
        val base = faceElapsed
        while (true) {
            val now = withFrameNanos { it }
            faceElapsed = base + ((now - start) / 1_000_000_000.0).toFloat()
        }
    }

    Box(
        modifier
            .size(width + bezel * 2, screenH + bezel * 2)
            .background(Color(0xFF1D1D1F), RoundedCornerShape(width * 0.155f)),
        contentAlignment = Alignment.Center,
    ) {
        val density = LocalDensity.current
        CompositionLocalProvider(
            LocalDensity provides Density(density.density, fontScale = 1f),
        ) {
            Box(
                Modifier
                    .size(width, screenH)
                    .clip(RoundedCornerShape(width * 0.115f))
                    .background(Palette.paper)
                    .alpha(1f - blank),
            ) {
                when (shown ?: face) {
                    AppFace.ROUTE -> RouteMockScreen(u, faceElapsed)
                    AppFace.MAPS -> MapsMockScreen(u, faceElapsed)
                    AppFace.RIDES -> RidesMockScreen(u)
                }
                // The dynamic island, so the glass reads as a phone.
                Box(
                    Modifier
                        .align(Alignment.TopCenter)
                        .padding(top = width * 0.028f)
                        .size(width * 0.29f, width * 0.075f)
                        .background(Color(0xFF1D1D1F), RoundedCornerShape(50)),
                )
            }
        }
    }
}

// MARK: - shared furniture

/** The nav bar every screen in the app has. */
@Composable
private fun PhoneTitle(text: String, u: Float) {
    Text(
        text,
        style = condensed((38 * u).sp, FontWeight.Bold),
        color = Palette.ink,
        modifier = Modifier
            .fillMaxWidth()
            .padding(start = (20 * u).dp, top = (56 * u).dp, bottom = (8 * u).dp),
    )
}

/** The tab bar, so the screens read as places in an app rather than as posters. */
@Composable
private fun PhoneTabs(active: Int, u: Float) {
    // The app's own four, in its own order. Maps isn't here because it isn't a
    // tab: it is a sheet off Settings.
    val tabs = listOf(
        Icons.Filled.Speed to "Ride",
        Icons.Filled.Map to "Route",
        Icons.AutoMirrored.Filled.ViewList to "Rides",
        Icons.Filled.Tune to "Settings",
    )
    Column {
        Box(Modifier.fillMaxWidth().height(1.dp).background(Palette.hairline))
        Row(
            Modifier
                .fillMaxWidth()
                .background(Palette.surface)
                .padding(top = (10 * u).dp, bottom = (20 * u).dp),
        ) {
            tabs.forEachIndexed { i, (icon, label) ->
                Column(
                    Modifier.weight(1f),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    Icon(
                        icon,
                        contentDescription = null,
                        tint = if (i == active) Palette.accent else Palette.faint,
                        modifier = Modifier.size((16 * u).dp),
                    )
                    Text(
                        label,
                        style = barlow((8 * u).sp, FontWeight.Medium),
                        color = if (i == active) Palette.accent else Palette.faint,
                        maxLines = 1,
                    )
                }
            }
        }
    }
}

/**
 * The map under everything, in the colours the app's map actually uses: warm land,
 * white roads by class, green for parks, grey-blue for water. Enough of a place
 * for a route to run through and for hexes to be picked off.
 */
@Composable
private fun MiniMap(
    u: Float,
    modifier: Modifier = Modifier,
    /** 0…1 of the planned route drawn in; null for no route. */
    route: Float? = null,
    /** 0…1 of the picked hexes already on the device; null when nothing is being
     *  sent. Hexes, not squares — the app picks H3 cells, and a grid of squares
     *  was the tell that this mock had been drawn from memory. */
    hexes: Float? = null,
    /** A ride card's thumbnail: bigger blocks, one recorded track, no chrome. */
    thumbnail: Boolean = false,
) {
    // Everything below is written in the same units as the real screens (dp),
    // but a DrawScope measures in raw pixels — so the whole drawing is scaled by
    // the display density. Without it the city block grid comes out three times
    // too fine and the map reads as graph paper.
    val px = u * LocalDensity.current.density
    Canvas(modifier) {
        val block = (if (thumbnail) 78f else 58f) * px
        drawBase(block, px)
        route?.let { drawRouteLine(block, px, it) }
        if (thumbnail) drawTrack(px)
        hexes?.let { drawHexes(px, it) }
    }
}

private fun DrawScope.drawBase(block: Float, u: Float) {
    // Land a clear step darker than the roads: at thumbnail size the two were
    // within a few percent of each other and the map read as blank paper with a
    // stripe on it.
    drawRect(Color(0xFFDED8CB))

    // Water along one corner and a park or two, so the map has landmarks rather
    // than being an endless even grid.
    val bay = Path().apply {
        moveTo(size.width, size.height * 0.16f)
        cubicTo(
            size.width * 0.88f, size.height * 0.06f,
            size.width * 0.74f, 0f,
            size.width * 0.58f, 0f,
        )
        lineTo(size.width, 0f)
        close()
    }
    drawPath(bay, Color(0xFFA8C7DA))

    for (r in listOf(
        Rect(block * 0.3f, size.height - block * 1.9f, block * 1.7f, size.height - block),
        Rect(
            size.width - block * 1.6f, size.height * 0.44f,
            size.width - block * 0.4f, size.height * 0.44f + block * 0.8f,
        ),
    )) {
        drawRoundRect(
            Color(0xFFBCD69F),
            topLeft = Offset(r.left, r.top),
            size = Size(r.width, r.height),
            cornerRadius = androidx.compose.ui.geometry.CornerRadius(3 * u),
        )
    }

    val minor = Path()
    val major = Path()
    var i = 0
    var x = block
    while (x < size.width) {
        // Every third street is an arterial, which is what keeps the grid from
        // reading as graph paper.
        val p = if (i % 3 == 0) major else minor
        p.moveTo(x, 0f); p.lineTo(x, size.height)
        x += block; i += 1
    }
    i = 0
    var y = block
    while (y < size.height) {
        val p = if (i % 3 == 1) major else minor
        p.moveTo(0f, y); p.lineTo(size.width, y)
        y += block; i += 1
    }
    drawPath(minor, Color.White, style = Stroke(width = 4 * u))
    drawPath(major, Color.White, style = Stroke(width = 7.5f * u))
}

/**
 * The planned route, following the streets and turning at junctions — a line that
 * ignores the roads under it is what makes a map look fake.
 */
private fun DrawScope.drawRouteLine(block: Float, u: Float, progress: Float) {
    val pts = listOf(
        Offset(block * 0.6f, size.height - block * 0.7f),
        Offset(block * 0.6f, size.height - block * 2.6f),
        Offset(block * 2.0f, size.height - block * 2.6f),
        Offset(block * 2.0f, size.height - block * 4.0f),
        Offset(block * 3.4f, size.height - block * 4.0f),
        Offset(block * 3.4f, size.height - block * 5.2f),
    )
    val lens = (1 until pts.size).map { abs(pts[it].x - pts[it - 1].x) + abs(pts[it].y - pts[it - 1].y) }
    var left = lens.sum() * progress

    val path = Path()
    path.moveTo(pts[0].x, pts[0].y)
    var end = pts[0]
    for (i in 1 until pts.size) {
        val len = lens[i - 1]
        if (left >= len) {
            path.lineTo(pts[i].x, pts[i].y)
            end = pts[i]
            left -= len
        } else {
            val f = if (len == 0f) 0f else left / len
            end = Offset(
                pts[i - 1].x + (pts[i].x - pts[i - 1].x) * f,
                pts[i - 1].y + (pts[i].y - pts[i - 1].y) * f,
            )
            path.lineTo(end.x, end.y)
            break
        }
    }
    drawPath(
        path,
        Palette.accent,
        style = Stroke(width = 5 * u, cap = StrokeCap.Round, join = StrokeJoin.Round),
    )
    dot(pts[0], 4.5f * u, Palette.good, u)      // where you set off
    if (progress > 0.98f) dot(end, 5.5f * u, Palette.accent, u)
}

/**
 * A recorded ride, for the list's thumbnails: a track across the city with the
 * green start dot the real cards show.
 */
private fun DrawScope.drawTrack(u: Float) {
    val a = Offset(size.width * 0.22f, size.height * 0.82f)
    val p = Path().apply {
        moveTo(a.x, a.y)
        cubicTo(
            size.width * 0.42f, size.height * 0.60f,
            size.width * 0.56f, size.height * 0.46f,
            size.width * 0.82f, size.height * 0.18f,
        )
    }
    drawPath(p, Palette.accent, style = Stroke(width = 4.5f * u, cap = StrokeCap.Round))
    dot(a, 4 * u, Palette.good, u)
}

/**
 * The hex picker: the area you tapped out, filling in as the app streams each cell
 * to the card. Cells already on the device go green, the ones still queued stay
 * orange, and everything outside the selection is the grid you could tap next.
 *
 * The selection is every cell within two rings of the middle, which is exactly 19
 * — the number the panel underneath claims. Picking the cells geometrically
 * instead gave a blob of a hundred under a label reading "19", which is the sort
 * of detail that makes a mock look drawn from memory.
 */
private fun DrawScope.drawHexes(u: Float, progress: Float) {
    val r = 26 * u
    val centre = Offset(size.width / 2, size.height * 0.5f)

    fun place(q: Int, s: Int) = Offset(
        centre.x + r * 1.5f * q,
        centre.y + r * sqrt(3f) * (s + q / 2f),
    )
    fun ring(q: Int, s: Int) = (abs(q) + abs(q + s) + abs(s)) / 2

    val reach = (maxOf(size.width, size.height) / r).toInt() + 1
    // Rings 0…2 are the selection, in the order the app sends them: the middle
    // first, then outward.
    val picked = (-2..2).flatMap { q -> (-2..2).map { q to it } }
        .filter { ring(it.first, it.second) <= 2 }
        .sortedBy { ring(it.first, it.second) }
    val sent = (picked.size * progress).roundToInt()

    for (q in -reach..reach) {
        for (sIdx in -reach..reach) {
            val c = place(q, sIdx)
            if (c.x < -r || c.x > size.width + r || c.y < -r || c.y > size.height + r) continue
            val hex = hexPath(c, r)
            val idx = picked.indexOf(q to sIdx)
            if (idx < 0) {
                drawPath(hex, Color.White.copy(alpha = 0.65f), style = Stroke(width = 1f))
                continue
            }
            val done = idx < sent
            val colour = if (done) Palette.good else Palette.accent
            drawPath(hex, colour.copy(alpha = 0.3f))
            drawPath(hex, colour, style = Stroke(width = 1.6f))
        }
    }
}

private fun hexPath(c: Offset, r: Float): Path {
    val p = Path()
    for (i in 0 until 6) {
        val a = i * PI.toFloat() / 3
        val pt = Offset(c.x + r * cos(a), c.y + r * sin(a))
        if (i == 0) p.moveTo(pt.x, pt.y) else p.lineTo(pt.x, pt.y)
    }
    p.close()
    return p
}

private fun DrawScope.dot(p: Offset, r: Float, fill: Color, u: Float) {
    drawCircle(fill, r, p)
    drawCircle(Color.White, r, p, style = Stroke(width = 1.6f * u))
}

// MARK: - the three screens

@Composable
private fun RouteMockScreen(u: Float, elapsed: Float) {
    // Draws in over a couple of seconds, then holds — long enough to watch it
    // happen, short enough that it has happened by the time you look up.
    val progress = ((elapsed - 0.3f) / 2.2f).coerceIn(0f, 1f)
    Column(Modifier.fillMaxSize()) {
        PhoneTitle("Route", u)
        Row(
            Modifier
                .padding(horizontal = (16 * u).dp)
                .fillMaxWidth()
                .background(Palette.surface, RoundedCornerShape((10 * u).dp))
                .border(1.dp, Palette.hairline, RoundedCornerShape((10 * u).dp))
                .padding(horizontal = (12 * u).dp, vertical = (9 * u).dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy((6 * u).dp),
        ) {
            Icon(
                Icons.Filled.Search,
                contentDescription = null,
                tint = Palette.faint,
                modifier = Modifier.size((11 * u).dp),
            )
            Text(
                "Fisherman's Wharf",
                style = barlow((12 * u).sp, FontWeight.Medium),
                color = Palette.ink,
            )
        }
        MiniMap(
            u,
            Modifier
                .padding(horizontal = (16 * u).dp, vertical = (10 * u).dp)
                .weight(1f)
                .fillMaxWidth()
                .clip(RoundedCornerShape((14 * u).dp)),
            route = progress,
        )
        Column(
            Modifier.padding(horizontal = (16 * u).dp, vertical = (12 * u).dp),
            verticalArrangement = Arrangement.spacedBy((8 * u).dp),
        ) {
            Row(verticalAlignment = Alignment.Bottom) {
                Text("12.2", style = condensed((30 * u).sp, FontWeight.Bold), color = Palette.ink)
                Spacer(Modifier.size((6 * u).dp))
                Text(
                    "KM",
                    style = barlow((11 * u).sp, FontWeight.SemiBold),
                    color = Palette.muted,
                )
                Spacer(Modifier.weight(1f))
                Text(
                    "46 min",
                    style = barlow((12 * u).sp, FontWeight.Medium),
                    color = Palette.muted,
                )
            }
            Text(
                if (progress < 1f) "Building route…" else "Connect to send",
                style = condensed((17 * u).sp, FontWeight.SemiBold),
                color = Palette.accentInk,
                modifier = Modifier
                    .fillMaxWidth()
                    .background(
                        if (progress < 1f) Palette.faint else Palette.accent,
                        RoundedCornerShape(50),
                    )
                    .padding(vertical = (10 * u).dp),
            )
        }
        PhoneTabs(active = 1, u = u)
    }
}

/**
 * Picking an area and streaming it to the card. The map fills the screen and the
 * hexes sit on top of it, because in the app the map IS the interface — you tap
 * cells on the place you are looking at.
 */
@Composable
private fun MapsMockScreen(u: Float, elapsed: Float) {
    val progress = ((elapsed - 0.4f) / 4.5f).coerceIn(0f, 1f)
    /** Rings 0…2 of the hex grid — the same 19 cells MiniMap fills in. */
    val picked = 19
    Column(Modifier.fillMaxSize()) {
        // A sheet, because that is how the app opens this: presented over
        // Settings, so it gets a grabber and no tab bar.
        Box(
            Modifier
                .align(Alignment.CenterHorizontally)
                .padding(top = (8 * u).dp)
                .size((34 * u).dp, (5 * u).dp)
                .background(Palette.hairline, RoundedCornerShape(50)),
        )
        PhoneTitle("Maps", u)
        MiniMap(
            u,
            Modifier
                .padding(horizontal = (16 * u).dp)
                .weight(1f)
                .fillMaxWidth()
                .clip(RoundedCornerShape((14 * u).dp)),
            hexes = progress,
        )
        Column(
            Modifier.padding(horizontal = (16 * u).dp, vertical = (12 * u).dp),
            verticalArrangement = Arrangement.spacedBy((7 * u).dp),
        ) {
            Text(
                "SELECTED AREA",
                style = barlow((9 * u).sp, FontWeight.SemiBold)
                    .copy(letterSpacing = (0.9f * u).sp),
                color = Palette.muted,
            )
            Row(verticalAlignment = Alignment.Bottom) {
                Text(
                    if (progress < 1f) "${(picked * progress).toInt()}" else "$picked",
                    style = condensed((26 * u).sp, FontWeight.Bold),
                    color = Palette.ink,
                )
                Spacer(Modifier.size((5 * u).dp))
                Text(
                    if (progress < 1f) "of $picked hexes sent" else "hexes on the device",
                    style = barlow((11 * u).sp, FontWeight.Medium),
                    color = Palette.muted,
                )
            }
            Box(
                Modifier
                    .fillMaxWidth()
                    .height((5 * u).dp)
                    .background(Palette.hairline, RoundedCornerShape(50)),
            ) {
                Box(
                    Modifier
                        .fillMaxWidth(progress)
                        .height((5 * u).dp)
                        .background(
                            if (progress < 1f) Palette.accent else Palette.good,
                            RoundedCornerShape(50),
                        ),
                )
            }
            Spacer(Modifier.height((14 * u).dp))
        }
    }
}

/**
 * The rides list, as it actually is: a week's totals, then a card per ride with
 * the map of where you went. The map is the whole point of the card — it is how
 * you recognise a ride before you have read a single number off it.
 */
@Composable
private fun RidesMockScreen(u: Float) {
    Column(Modifier.fillMaxSize()) {
        PhoneTitle("Rides", u)
        Column(
            Modifier
                .padding(horizontal = (16 * u).dp)
                .weight(1f)
                .clipToBounds(),
            verticalArrangement = Arrangement.spacedBy((10 * u).dp),
        ) {
            Row(
                Modifier
                    .fillMaxWidth()
                    .background(Palette.surface, RoundedCornerShape((12 * u).dp))
                    .border(1.dp, Palette.hairline, RoundedCornerShape((12 * u).dp))
                    .padding(horizontal = (12 * u).dp, vertical = (10 * u).dp),
            ) {
                WeekCell("3", "rides this week", u, Modifier.weight(1f))
                WeekCell("14.4", "km distance", u, Modifier.weight(1f))
                WeekCell("29m", "riding time", u, Modifier.weight(1f))
            }
            RideMockCard("Sunday night ride", "Jul 19 · 12:15 AM", "4.8", "9m", "5.1", "219", u)
            RideMockCard("Saturday morning ride", "Jul 18 · 9:30 AM", "12.6", "34m", "22.2", "204", u)
            RideMockCard("Thursday commute", "Jul 16 · 8:02 AM", "9.1", "26m", "21.0", "188", u)
            RideMockCard("Tuesday intervals", "Jul 14 · 6:40 PM", "31.7", "1:04", "29.7", "263", u)
        }
        PhoneTabs(active = 2, u = u)
    }
}

@Composable
private fun WeekCell(value: String, label: String, u: Float, modifier: Modifier = Modifier) {
    Column(modifier) {
        Text(value, style = condensed((21 * u).sp, FontWeight.Bold), color = Palette.ink)
        Text(label, style = barlow((8 * u).sp), color = Palette.muted, maxLines = 1)
    }
}

@Composable
private fun RideMockCard(
    title: String,
    when_: String,
    distance: String,
    time: String,
    speed: String,
    power: String,
    u: Float,
) {
    Column(
        Modifier
            .fillMaxWidth()
            .background(Palette.surface, RoundedCornerShape((12 * u).dp))
            .border(1.dp, Palette.hairline, RoundedCornerShape((12 * u).dp))
            .padding((9 * u).dp),
        verticalArrangement = Arrangement.spacedBy((7 * u).dp),
    ) {
        MiniMap(
            u,
            Modifier
                .fillMaxWidth()
                .height((88 * u).dp)
                .clip(RoundedCornerShape((8 * u).dp)),
            thumbnail = true,
        )
        Row(
            horizontalArrangement = Arrangement.spacedBy((5 * u).dp),
            verticalAlignment = Alignment.Bottom,
        ) {
            Text(
                title,
                style = barlow((11 * u).sp, FontWeight.SemiBold),
                color = Palette.ink,
                maxLines = 1,
            )
            Text(when_, style = barlow((8 * u).sp), color = Palette.muted, maxLines = 1)
        }
        Row(Modifier.fillMaxWidth()) {
            MockStat(distance, "Distance", u, Modifier.weight(1f))
            MockStat(time, "Time", u, Modifier.weight(1f))
            MockStat(speed, "km/h", u, Modifier.weight(1f))
            MockStat(power, "Power", u, Modifier.weight(1f))
        }
    }
}

@Composable
private fun MockStat(value: String, label: String, u: Float, modifier: Modifier = Modifier) {
    Column(modifier) {
        Text(
            value,
            style = condensed((17 * u).sp, FontWeight.Bold),
            color = Palette.ink,
            maxLines = 1,
        )
        Text(label, style = barlow((7.5f * u).sp), color = Palette.muted, maxLines = 1)
    }
}
