package com.raemond.opentrailpaper.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
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
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.KeyboardArrowUp
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Remove
import androidx.compose.material.icons.filled.SkipNext
import androidx.compose.material.icons.filled.SkipPrevious
import androidx.compose.material3.Icon
import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.BlendMode
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.FilterQuality
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.layout.FirstBaseline
import androidx.compose.ui.layout.Layout
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.data.DashField
import com.raemond.opentrailpaper.data.Units
import kotlinx.coroutines.delay
import java.util.Locale
import kotlin.math.abs
import kotlin.math.roundToInt

// The device's non-field pages — MUSIC, MAP, WORKOUT, STOPWATCH — drawn in the
// panel's own pixels (540 x 960, ui_render.h) and scaled once, the same idiom
// as DashPreview. One renderer per page, used twice: the dashboard editor's
// carousel cards (sample data, the defaults below) and the Ride tab's live
// copy of the device (the rider's data). Positions follow ui_render.cpp, which
// places text by its BASELINE; [PanelText] does the same. Port of
// companion-ios/Sources/DevicePages.swift.

/** What the device's status bar shows. */
data class PanelStatus(
    val clock: String = "14:25",
    val satellites: Int = 11,
    val battery: Int = 76,
    val linked: Boolean = true,
    val sensors: String = "· HR · PWR",
) {
    companion object {
        val sample = PanelStatus()
    }
}

/**
 * The firmware's faces, at the sizes their ascenders give: Arial_L/B for text,
 * Impact_T/H for numbers, drawn with the bundled Barlow / Barlow Condensed.
 * [digits] is a value face whose '0' is `px` panel pixels tall.
 */
class PanelFont(val family: FontFamily, val weight: FontWeight, val size: Float) {
    fun style(k: Float) = TextStyle(fontFamily = family, fontWeight = weight, fontSize = (size * k).sp)

    /** A rough advance in panel pixels, for laying out columns. */
    fun width(s: String): Float = s.length * size * (if (family == Barlow) 0.52f else 0.42f)

    companion object {
        /** Barlow Condensed Bold's '0' ink height per point of size. */
        private const val DIGIT_PER_PT = 0.722f
        val arialL = PanelFont(Barlow, FontWeight.Medium, 20f)
        val arialB = PanelFont(Barlow, FontWeight.Bold, 27f)
        val impactT = PanelFont(BarlowCondensed, FontWeight.Bold, 54f)
        val impactH = PanelFont(BarlowCondensed, FontWeight.Bold, 160f)
        fun digits(px: Float) = PanelFont(BarlowCondensed, FontWeight.Bold, px / DIGIT_PER_PT)
    }
}

enum class PanelAlign { LEFT, RIGHT, CENTER }

private val DARK = Color.Black.copy(alpha = 0.6f)

/**
 * Text at a device-pixel baseline, aligned like epd text: left from [x], right
 * to [x], or centred on [x]. [maxWidth] truncates with an ellipsis, as the
 * firmware's truncToWidth does. Fills its parent and places itself.
 */
@Composable
fun PanelText(
    text: String,
    font: PanelFont,
    x: Float,
    baseline: Float,
    k: Float,
    align: PanelAlign = PanelAlign.LEFT,
    color: Color = Color.Black,
    maxWidth: Float? = null,
) {
    Layout(
        content = {
            Text(
                text,
                style = font.style(k),
                color = color,
                maxLines = 1,
                softWrap = false,
                overflow = if (maxWidth != null) TextOverflow.Ellipsis else TextOverflow.Clip,
            )
        },
        modifier = Modifier.fillMaxSize(),
    ) { measurables, constraints ->
        val maxW = maxWidth?.let { (it * k).dp.roundToPx() } ?: Constraints.Infinity
        val p = measurables[0].measure(Constraints(maxWidth = maxW))
        val base = p[FirstBaseline].takeIf { it != androidx.compose.ui.layout.AlignmentLine.Unspecified } ?: p.height
        val xPx = (x * k).dp.toPx()
        val left = when (align) {
            PanelAlign.LEFT -> xPx
            PanelAlign.RIGHT -> xPx - p.width
            PanelAlign.CENTER -> xPx - p.width / 2f
        }
        val w = if (constraints.hasBoundedWidth) constraints.maxWidth else p.width
        val h = if (constraints.hasBoundedHeight) constraints.maxHeight else p.height
        layout(w, h) {
            p.place(left.roundToInt(), ((baseline * k).dp.toPx() - base).roundToInt())
        }
    }
}

/** A device rectangle at panel coordinates: [fill] inside a [line]-px outline (0 = none). */
@Composable
fun PanelRect(
    x: Float, y: Float, w: Float, h: Float, k: Float,
    line: Float = 3f,
    fill: Color = Color.Transparent,
    edge: Color = Color.Black,
) {
    Box(
        Modifier
            .offset((x * k).dp, (y * k).dp)
            .size((w * k).dp, (h * k).dp)
            .background(fill)
            .then(if (line > 0f) Modifier.border(maxOf(line * k, 0.6f).dp, edge) else Modifier),
    )
}

@Composable
private fun PanelFill(x: Float, y: Float, w: Float, h: Float, k: Float, color: Color = Color.Black) =
    PanelRect(x, y, w, h, k, line = 0f, fill = color)

@Composable
private fun PanelIcon(icon: ImageVector, x: Float, y: Float, w: Float, h: Float, k: Float, size: Float = 40f, alpha: Float = 1f) {
    Box(
        Modifier.offset((x * k).dp, (y * k).dp).size((w * k).dp, (h * k).dp),
        contentAlignment = Alignment.Center,
    ) {
        Icon(icon, contentDescription = null, tint = Color.Black.copy(alpha = alpha), modifier = Modifier.size((size * k).dp))
    }
}

/**
 * A touch target over a control the panel draws, at the device's own rect
 * (ui_render.h) scaled into the preview, so a tap lands where the button is
 * drawn. Pressing inverts the rect, the device's own tap feedback.
 */
@Composable
fun PanelTap(
    x: Float, y: Float, w: Float, h: Float, k: Float,
    label: String,
    enabled: Boolean = true,
    onTap: () -> Unit,
) {
    val source = remember { MutableInteractionSource() }
    val pressed by source.collectIsPressedAsState()
    var flash by remember { mutableStateOf(false) }
    LaunchedEffect(flash) {
        if (flash) {
            delay(180)
            flash = false
        }
    }
    Box(
        Modifier
            .offset((x * k).dp, (y * k).dp)
            .size((w * k).dp, (h * k).dp)
            .drawWithContent {
                drawContent()
                if (pressed || flash) drawRect(Color.White, blendMode = BlendMode.Difference)
            }
            .clickable(
                interactionSource = source,
                indication = null,
                enabled = enabled,
                role = Role.Button,
                onClickLabel = label,
            ) {
                flash = true
                onTap()
            }
            .semantics { contentDescription = label },
    )
}

/** The device's status bar: clock, phone link, satellites, sensors (or the
 *  page's title), the battery. Same order and abbreviations as the panel. */
@Composable
fun DeviceStatusBar(status: PanelStatus, k: Float, title: String? = null, modifier: Modifier = Modifier) {
    Row(
        modifier
            .fillMaxWidth()
            .height((61 * k).dp)
            .padding(horizontal = (18 * k).dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy((9 * k).dp),
    ) {
        Text(status.clock, style = condensed((40 * k).sp, FontWeight.Bold), color = Color.Black, maxLines = 1)
        if (status.linked) {
            Box(
                Modifier
                    .size((15 * k).dp, (26 * k).dp)
                    .border((3 * k).dp, Color.Black, RoundedCornerShape((3 * k).dp)),
            )
        }
        // One dot per satellite past eight, capped at four; empty dots stay as
        // outlines so the bar never reflows.
        Row(horizontalArrangement = Arrangement.spacedBy((4 * k).dp)) {
            val lit = (status.satellites - 8).coerceIn(0, 4)
            repeat(4) { i ->
                Box(
                    Modifier
                        .size((9 * k).dp)
                        .background(if (i < lit) Color.Black else Color.Transparent, CircleShape)
                        .border((1.5f * k).dp, Color.Black, CircleShape),
                )
            }
        }
        if (title != null) {
            Spacer(Modifier.weight(1f))
            Text(
                title, style = condensed((30 * k).sp, FontWeight.Bold).copy(letterSpacing = (5 * k).sp),
                color = Color.Black, maxLines = 1,
            )
        } else {
            Text(status.sensors, style = condensed((36 * k).sp, FontWeight.Bold), color = Color.Black, maxLines = 1)
        }
        Spacer(Modifier.weight(1f))
        Text("${status.battery}%", style = condensed((36 * k).sp, FontWeight.Bold), color = Color.Black, maxLines = 1)
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
                        .width((37 * k * status.battery.coerceIn(0, 100) / 100f).dp)
                        .background(Color.Black),
                )
            }
            Spacer(Modifier.width((1.5f * k).dp))
            Box(Modifier.size((4 * k).dp, (10 * k).dp).background(Color.Black))
        }
    }
}

/**
 * A device screen: [content] drawn in panel pixels with scale `k`, the status
 * bar and its rule on top. Always the panel's 9:16; font scale pinned, since
 * panel geometry is absolute.
 */
@Composable
fun DevicePanel(
    modifier: Modifier = Modifier,
    status: PanelStatus = PanelStatus.sample,
    title: String? = null,
    content: @Composable BoxScope.(k: Float) -> Unit,
) {
    BoxWithConstraints(
        modifier
            .aspectRatio(PANEL_W / PANEL_H)
            .background(PanelPaper)
            .clipToBounds(),
    ) {
        val k = maxWidth.value / PANEL_W
        val density = LocalDensity.current
        CompositionLocalProvider(
            LocalDensity provides Density(density.density, fontScale = 1f),
            LocalTextStyle provides LocalTextStyle.current.copy(color = Color.Black),
        ) {
            Box(Modifier.fillMaxSize()) {
                content(k)
                DeviceStatusBar(status, k, title, Modifier.background(PanelPaper))
                PanelFill(0f, 61f, 540f, 3f, k)
            }
        }
    }
}

/** The device's pager (ui_render_pager): page up / down, "1-8 OF 12", and an action button. */
@Composable
private fun PanelPager(
    first: Int, count: Int, total: Int, action: String, k: Float,
    onUp: (() -> Unit)? = null,
    onDown: (() -> Unit)? = null,
    onAction: (() -> Unit)? = null,
) {
    PanelFill(0f, 857f, 540f, 103f, k, PanelPaper)
    PanelFill(0f, 857f, 540f, 3f, k)
    if (total > 0) {
        PanelRect(24f, 868f, 80f, 80f, k)
        PanelIcon(Icons.Filled.KeyboardArrowUp, 24f, 868f, 80f, 80f, k, 56f, if (first > 0) 1f else 0.3f)
        PanelRect(116f, 868f, 80f, 80f, k)
        PanelIcon(Icons.Filled.KeyboardArrowDown, 116f, 868f, 80f, 80f, k, 56f, if (first + count < total) 1f else 0.3f)
        if (count > 0) {
            PanelText("${first + 1}-${first + count} OF $total", PanelFont.arialB, 282f, 914f, k, PanelAlign.CENTER)
        }
    }
    PanelRect(368f, 868f, 148f, 80f, k)
    PanelText(action, PanelFont.arialB, 442f, 914f, k, PanelAlign.CENTER)
    if (onUp != null || onDown != null || onAction != null) {
        if (total > 0) {
            PanelTap(24f, 868f, 80f, 80f, k, "Page up", enabled = first > 0) { onUp?.invoke() }
            PanelTap(116f, 868f, 80f, 80f, k, "Page down", enabled = first + count < total) { onDown?.invoke() }
        }
        PanelTap(368f, 868f, 148f, 80f, k, action.lowercase().replaceFirstChar { it.uppercase() }) { onAction?.invoke() }
    }
}

/** Hatching in panel pixels, for done blocks and the redo strip. */
private fun Modifier.panelHatch(k: Float): Modifier =
    hatched(spacing = (6 * k).dp, lineWidth = maxOf(1.2f * k, 0.5f).dp, color = Color.Black, background = Color.Transparent)

// MARK: - Sample text shared by the cards

fun previewPanelLabel(id: String): String =
    if (id == "power3s") "POWER · 3S" else (DashField.named(id)?.label ?: id).uppercase()

fun previewSample(id: String): String = when (id) {
    "speed" -> "32.4"
    "power3s", "power" -> "247"
    "hr" -> "156"
    "cadence" -> "88"
    "distance" -> "54.8"
    "ridetime", "movingtime" -> "1:47"
    "climb" -> "918"
    "grade" -> "4.2"
    "altitude" -> "112"
    "battery" -> "76"
    "sats" -> "11"
    "clock" -> "14:25"
    "routeleft" -> "12.4"
    else -> "--"
}

// MARK: - MAP

/**
 * The device's MAP page: the map band, its zoom stack, and the configured
 * 3-field data strip. [map] fills the band — the Ride tab passes the live map;
 * the editor card keeps a drawn street grid.
 */
@Composable
fun MapPagePreview(
    fields: List<String>,
    modifier: Modifier = Modifier,
    status: PanelStatus = PanelStatus.sample,
    value: (String) -> String = ::previewSample,
    map: (@Composable (k: Float) -> Unit)? = null,
    /** +1 zoom in, -1 out. The zoom stack's rects, (462, 560/640, 76, 76). */
    onZoom: ((Int) -> Unit)? = null,
) {
    DevicePanel(modifier, status) { k ->
        Box(Modifier.offset(y = (64 * k).dp).size((540 * k).dp, (746 * k).dp).clipToBounds()) {
            if (map != null) map(k) else SampleGrid(k)
        }
        for (v in 0 until 2) {
            PanelRect(462f, 560f + v * 80f, 76f, 76f, k, fill = PanelPaper)
            PanelIcon(if (v == 0) Icons.Filled.Add else Icons.Filled.Remove, 462f, 560f + v * 80f, 76f, 76f, k, 44f)
        }
        if (onZoom != null) {
            PanelTap(462f, 560f, 76f, 76f, k, "Zoom in") { onZoom(1) }
            PanelTap(462f, 640f, 76f, 76f, k, "Zoom out") { onZoom(-1) }
        }
        // Data strip: the CONFIGURED fields, drawn like dash cells.
        PanelFill(0f, 810f, 540f, 150f, k, PanelPaper)
        PanelFill(0f, 810f, 540f, 3f, k)
        for (c in 0 until 3) {
            val id = fields.getOrElse(c) { "speed" }
            val x = 24f + c * 168f
            PanelRect(x, 825f, 156f, 123f, k, line = 2f)
            Text(
                previewPanelLabel(id),
                style = barlow((15 * k).sp, FontWeight.SemiBold).copy(letterSpacing = (2 * k).sp),
                maxLines = 1, overflow = TextOverflow.Ellipsis,
                modifier = Modifier.offset(((x + 14) * k).dp, (837 * k).dp).width((130 * k).dp),
            )
            Box(
                Modifier.offset((x * k).dp, (848 * k).dp).size((156 * k).dp, (100 * k).dp),
                contentAlignment = Alignment.Center,
            ) {
                Text(value(id), style = condensed((52 * k).sp, FontWeight.Bold), maxLines = 1)
            }
        }
    }
}

/** The editor card's stand-in map: a rotated street grid and a position. */
@Composable
private fun SampleGrid(k: Float) {
    Canvas(Modifier.fillMaxSize()) {
        val s = k * density
        rotate(18f, pivot = Offset(270f * s, 366f * s)) {
            for (i in 0 until 4) {
                drawRect(
                    Color.Black.copy(alpha = if (i == 1) 0.9f else 0.35f),
                    Offset(-80f * s, (86f + i * 150f) * s),
                    Size(700f * s, (if (i == 1) 7f else 3f) * s),
                )
            }
            for (i in 0 until 3) {
                drawRect(Color.Black.copy(alpha = 0.35f), Offset((120f + i * 160f) * s, 16f * s), Size(3f * s, 700f * s))
            }
        }
        drawCircle(Color.Black, 13f * s, Offset(270f * s, 369f * s))
        drawCircle(Color.Black, 22f * s, Offset(270f * s, 369f * s), style = Stroke((2f * s).coerceAtLeast(1f)))
    }
}

/**
 * The device's rider marker (map_view.cpp drawRider): a white disc with a 3px
 * ring and a split arrowhead along the course. No course yet: the arrow points up.
 */
@Composable
fun RiderMark(headingDeg: Double?, k: Float, modifier: Modifier = Modifier) {
    Canvas(modifier.size((64 * k).dp)) {
        val s = k * density
        val c = Offset(size.width / 2, size.height / 2)
        drawCircle(Color.White, 30f * s, c)
        drawCircle(Color.Black, 29f * s, c, style = Stroke((3f * s).coerceAtLeast(1f)))
        val rad = Math.toRadians((headingDeg ?: 0.0) - 90.0)
        val cs = kotlin.math.cos(rad).toFloat()
        val sn = kotlin.math.sin(rad).toFloat()
        fun rot(lx: Float, ly: Float) = Offset(c.x + (lx * cs - ly * sn) * s, c.y + (lx * sn + ly * cs) * s)
        val arrow = Path().apply {
            val a = rot(24f, 0f); moveTo(a.x, a.y)
            val b = rot(-15f, -13f); lineTo(b.x, b.y)
            val m = rot(-8f, 0f); lineTo(m.x, m.y)
            val d = rot(-15f, 13f); lineTo(d.x, d.y)
            close()
        }
        drawPath(arrow, Color.Black)
    }
}

/** The device's compass (map_view.cpp drawCompass): ringed disc, needle to north — up. */
@Composable
fun CompassMark(k: Float, modifier: Modifier = Modifier) {
    Canvas(modifier.size((60 * k).dp)) {
        val s = k * density
        val c = Offset(size.width / 2, size.height / 2)
        drawCircle(Color.White, 28f * s, c)
        drawCircle(Color.Black, 27f * s, c, style = Stroke((3f * s).coerceAtLeast(1f)))
        val needle = Path().apply {
            moveTo(c.x, c.y - 19f * s)
            lineTo(c.x + 7f * s, c.y)
            lineTo(c.x - 7f * s, c.y)
            close()
        }
        drawPath(needle, Color.Black)
    }
}

// MARK: - MUSIC

/** A track as the MUSIC page prints it. */
data class PanelTrack(val title: String, val artist: String, val album: String)

private val SAMPLE_TRACK = PanelTrack("Turn Your Lights Down Low", "Bob Marley & The Wailers", "Exodus")

/**
 * The device's MUSIC page (ui_render_music): art (or a record when there is
 * none), title / artist / album, progress, transport and volume.
 */
@Composable
fun MusicPreview(
    modifier: Modifier = Modifier,
    status: PanelStatus = PanelStatus.sample,
    /** null = NOTHING PLAYING. */
    track: PanelTrack? = SAMPLE_TRACK,
    /** The cover as the device draws it (pre-dithered to its five tones). */
    art: ImageBitmap? = null,
    playing: Boolean = true,
    /** Position / length in seconds, when known. */
    progress: Pair<Int, Int>? = 154 to 331,
    /** Transport taps (0 prev, 1 play/pause, 2 next); null = not interactive. */
    onTransport: ((Int) -> Unit)? = null,
    transportEnabled: Boolean = true,
    /** Volume taps (0 up, 1 down); null = drawn but dead. */
    onVolume: ((Int) -> Unit)? = null,
) {
    DevicePanel(modifier, status, title = "MUSIC") { k ->
        PanelRect(108f, 96f, 324f, 324f, k, line = 2f)
        if (art != null) {
            Image(
                art, contentDescription = "Album art",
                filterQuality = FilterQuality.None,
                modifier = Modifier.offset((120 * k).dp, (108 * k).dp).size((300 * k).dp),
            )
        } else {
            Canvas(Modifier.fillMaxSize()) {
                val s = k * density
                val c = Offset(270f * s, 258f * s)
                drawCircle(Color.Black, 104f * s, c, style = Stroke((2f * s).coerceAtLeast(1f)))
                drawCircle(Color.Black, 34f * s, c)
                drawCircle(PanelPaper, 12f * s, c)
            }
        }
        // Volume, zoom-button style.
        PanelText("VOL", PanelFont.arialL, 500f, 128f, k, PanelAlign.CENTER)
        val volAlpha = if (onTransport != null && onVolume == null) 0.35f else 1f
        for (v in 0 until 2) {
            Box(Modifier.alpha(volAlpha)) {
                PanelRect(462f, 140f + v * 92f, 76f, 76f, k)
                PanelIcon(if (v == 0) Icons.Filled.Add else Icons.Filled.Remove, 462f, 140f + v * 92f, 76f, 76f, k, 44f)
            }
        }
        if (track != null) {
            PanelText(track.title.ifEmpty { "UNTITLED" }.uppercase(), PanelFont.impactT, 270f, 508f, k, PanelAlign.CENTER, maxWidth = 492f)
            PanelText(track.artist, PanelFont.arialB, 270f, 556f, k, PanelAlign.CENTER, maxWidth = 492f)
            PanelText(track.album, PanelFont.arialL, 270f, 592f, k, PanelAlign.CENTER, DARK, maxWidth = 492f)
        } else {
            PanelText("NOTHING PLAYING", PanelFont.impactT, 270f, 508f, k, PanelAlign.CENTER)
            PanelText("Start playback on your phone", PanelFont.arialB, 270f, 556f, k, PanelAlign.CENTER, DARK)
        }
        PanelRect(24f, 640f, 492f, 14f, k, line = 1f)
        if (progress != null && track != null && progress.second > 0) {
            val (pos, dur) = progress
            PanelFill(26f, 642f, 488f * minOf(pos, dur) / dur, 10f, k)
            PanelText(mmss(pos), PanelFont.arialL, 24f, 690f, k, color = DARK)
            PanelText(mmss(dur), PanelFont.arialL, 516f, 690f, k, PanelAlign.RIGHT, DARK)
        }
        val xs = floatArrayOf(24f, 176f, 376f)
        val ws = floatArrayOf(140f, 188f, 140f)
        val icons = listOf(Icons.Filled.SkipPrevious, if (playing) Icons.Filled.Pause else Icons.Filled.PlayArrow, Icons.Filled.SkipNext)
        val tAlpha = if (onTransport != null && !transportEnabled) 0.35f else 1f
        for (b in 0 until 3) {
            Box(Modifier.alpha(tAlpha)) {
                PanelRect(xs[b], 780f, ws[b], 120f, k, line = 2f)
                PanelIcon(icons[b], xs[b], 780f, ws[b], 120f, k, 56f)
            }
        }
        if (onTransport != null) {
            val labels = listOf("Previous track", if (playing) "Pause" else "Play", "Next track")
            for (b in 0 until 3) {
                PanelTap(xs[b], 780f, ws[b], 120f, k, labels[b], enabled = transportEnabled) { onTransport(b) }
            }
            for (v in 0 until 2) {
                PanelTap(462f, 140f + v * 92f, 76f, 76f, k, if (v == 0) "Volume up" else "Volume down", enabled = onVolume != null) {
                    onVolume?.invoke(v)
                }
            }
        }
    }
}

private fun mmss(s: Int) = String.format(Locale.US, "%d:%02d", s / 60, s % 60)

/** Panel tones (media.cpp kLum), for showing dithered art as the device draws it. */
private val TONE_LUM = intArrayOf(0, 70, 120, 170, 255)

/** 8-bit grey -> the device's five tones -> an image, so the phone shows the
 *  cover exactly as the panel draws it. */
fun panelArt(gray: ByteArray, side: Int): ImageBitmap? {
    if (gray.size != side * side) return null
    val tones = com.raemond.opentrailpaper.ble.ArtDither.dither(gray, side, side)
    val px = IntArray(tones.size) {
        val l = TONE_LUM[tones[it].toInt().coerceIn(0, 4)]
        (0xFF shl 24) or (l shl 16) or (l shl 8) or l
    }
    val bmp = android.graphics.Bitmap.createBitmap(px, side, side, android.graphics.Bitmap.Config.ARGB_8888)
    return bmp.asImageBitmap()
}

// MARK: - STOPWATCH

/** One finished lap as the STOPWATCH page's table lists it. */
data class StopwatchLapRow(
    val number: Int,
    val splitMs: Long,
    val speedKmh: Double?,
    val cadence: Int?,
    val powerW: Int?,
)

/**
 * What the STOPWATCH page draws once a lap is done (ui_render_stopwatch's lap
 * layout): the total, the lap in progress with its averages, and the table of
 * completed laps.
 */
data class StopwatchLapPage(
    val totalMs: Long,
    val running: Boolean,
    val lapNumber: Int,
    val lapMs: Long,
    val lapSpeedKmh: Double?,
    val lapCadence: Int?,
    val lapPowerW: Int?,
    val showCadence: Boolean,
    val showPower: Boolean,
    val useMiles: Boolean = false,
    /** Completed laps, newest first; null: the firmware doesn't send them. */
    val rows: List<StopwatchLapRow>?,
)

private val swBtnX = floatArrayOf(24f, 240f, 384f)
private val swBtnW = floatArrayOf(204f, 132f, 132f)

/**
 * The device's STOPWATCH page (ui_render_stopwatch). No laps: the time across
 * the middle, what it is doing, and START/PAUSE · RESET · LAP. With [lapPage],
 * the layout the device switches to after LAP.
 */
@Composable
fun StopwatchPagePreview(
    modifier: Modifier = Modifier,
    status: PanelStatus = PanelStatus.sample,
    time: String = "12:34",
    state: String = "RUNNING",
    running: Boolean = true,
    lapPage: StopwatchLapPage? = null,
    /** A line under the state, for the Ride tab to say why it shows "--". */
    note: String? = null,
    /** false: the buttons are drawn greyed and do nothing. */
    controlsEnabled: Boolean = true,
    /** The device's own rules: RESET only while paused with time, LAP only while running. */
    canReset: Boolean? = null,
    canLap: Boolean? = null,
    /** Button taps (0 START/PAUSE, 1 RESET, 2 LAP); null = not interactive. */
    onControl: ((Int) -> Unit)? = null,
) {
    fun enabled(i: Int) = when (i) {
        1 -> canReset ?: !running
        2 -> canLap ?: running
        else -> true
    }
    DevicePanel(modifier, status, title = "STOPWATCH") { k ->
        if (lapPage != null) {
            StopwatchLapLayout(lapPage, k)
        } else {
            Box(Modifier.offset(y = (250 * k).dp).width((540 * k).dp), contentAlignment = Alignment.Center) {
                Text(time, style = condensed((190 * k).sp, FontWeight.Bold), maxLines = 1)
            }
            PanelText(state, PanelFont.arialB, 270f, 530f, k, PanelAlign.CENTER)
            if (note != null) PanelText(note, PanelFont.arialL, 270f, 570f, k, PanelAlign.CENTER, DARK, maxWidth = 500f)
        }
        PanelFill(0f, 756f, 540f, 3f, k)
        val labels = listOf(if (running) "PAUSE" else "START", "RESET", "LAP")
        for (i in 0 until 3) {
            val on = enabled(i)
            Box(Modifier.alpha(if (controlsEnabled) 1f else 0.35f)) {
                PanelRect(swBtnX[i], 780f, swBtnW[i], 156f, k, line = if (on) 3f else 1f)
                Box(
                    Modifier.offset((swBtnX[i] * k).dp, (780 * k).dp).size((swBtnW[i] * k).dp, (156 * k).dp),
                    contentAlignment = Alignment.Center,
                ) {
                    Text(
                        labels[i], style = condensed((30 * k).sp, FontWeight.Bold),
                        color = Color.Black.copy(alpha = if (on) 1f else 0.35f),
                    )
                }
            }
        }
        if (onControl != null) {
            val names = listOf(if (running) "Pause" else "Start", "Reset", "Lap")
            for (i in 0 until 3) {
                PanelTap(swBtnX[i], 780f, swBtnW[i], 156f, k, names[i], enabled = controlsEnabled && enabled(i)) { onControl(i) }
            }
        }
    }
}

/** The running time (ui_render swFmt): MM:SS, H:MM:SS past the hour. */
fun stopwatchTimeText(ms: Long): String {
    val sec = ms / 1000
    return if (sec >= 3600) String.format(Locale.US, "%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60)
    else String.format(Locale.US, "%02d:%02d", sec / 60, sec % 60)
}

/** A finished lap (swFmtLap): M:SS.t under the hour, H:MM:SS over. */
private fun lapText(ms: Long): String {
    val sec = ms / 1000
    return if (sec >= 3600) String.format(Locale.US, "%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60)
    else String.format(Locale.US, "%d:%02d.%d", sec / 60, sec % 60, (ms % 1000) / 100)
}

/** ui_render_stopwatch's lap layout, at its pixel positions. */
@Composable
private fun StopwatchLapLayout(p: StopwatchLapPage, k: Float) {
    val right = 516f
    val listTop = 344f
    val rowTop = 384f
    val rowH = 52f
    val maxRows = 7
    val colsLeft = 72f
    val avgLeft = 160f
    // Metric columns: speed always; cadence and power as the device shows them.
    val metrics = buildList {
        add(0)
        if (p.showCadence) add(1)
        if (p.showPower) add(2)
    }
    fun unit(m: Int) = when (m) {
        0 -> Units.speedLabel(p.useMiles).uppercase()
        1 -> "RPM"
        else -> "W"
    }
    fun value(m: Int, speed: Double?, cadence: Int?, power: Int?) = when (m) {
        0 -> speed?.let { String.format(Locale.US, "%.1f", Units.speed(it, p.useMiles)) } ?: "--"
        1 -> cadence?.toString() ?: "--"
        else -> power?.toString() ?: "--"
    }
    // The total's face: the largest no taller than 95 px whose widest time fits.
    val tf = if (p.totalMs / 1000 >= 36000) 58f else if (p.totalMs / 1000 >= 3600) 81f else 95f
    val totalBase = 64f + 24f + tf
    PanelText("TOTAL", PanelFont.arialB, 24f, totalBase - 30f, k)
    PanelText(if (p.running) "RUNNING" else "PAUSED", PanelFont.arialL, 24f, totalBase, k, color = DARK)
    PanelText(stopwatchTimeText(p.totalMs), PanelFont.digits(tf), right, totalBase, k, PanelAlign.RIGHT)

    PanelText("LAP ${p.lapNumber}", PanelFont.arialB, 24f, 254f, k)
    PanelText(if (p.running) "IN PROGRESS" else "PAUSED", PanelFont.arialL, 24f, 284f, k, color = DARK)
    PanelText(stopwatchTimeText(p.lapMs), PanelFont.digits(81f), right, 284f, k, PanelAlign.RIGHT)
    PanelText("LAP AVERAGE", PanelFont.arialL, 24f, 328f, k, color = DARK)
    val share = ((right - avgLeft) / metrics.size).toInt().toFloat()
    metrics.forEachIndexed { i, m ->
        val v = value(m, p.lapSpeedKmh, p.lapCadence, p.lapPowerW)
        val u = unit(m)
        val uw = PanelFont.arialL.width(u)
        val w = PanelFont.digits(30f).width(v) + 6 + uw
        val cx = avgLeft + share * i + (share / 2).toInt()
        val r = (cx + w / 2).toInt().toFloat()
        PanelText(v, PanelFont.digits(30f), r - uw - 6, 328f, k, PanelAlign.RIGHT)
        PanelText(u, PanelFont.arialL, r, 328f, k, PanelAlign.RIGHT, DARK)
    }
    PanelFill(0f, listTop, 540f, 2f, k)

    val cols = 1 + metrics.size
    val step = ((right - colsLeft) / cols).toInt().toFloat()
    fun colRight(c: Int) = if (c == cols - 1) right else colsLeft + step * (c + 1)
    val shown = (p.rows ?: emptyList()).take(maxRows)
    val hourLap = shown.any { it.splitMs >= 3_600_000 }
    val rf = listOf(38f, 34f, 30f, 26f, 22f).firstOrNull {
        PanelFont.digits(it).width(if (hourLap) "0:00:00" else "00:00.0") <= step - 10
    } ?: 22f
    val capBase = rowTop - 12
    PanelText("LAP", PanelFont.arialL, 24f, capBase, k, color = DARK)
    PanelText("SPLIT", PanelFont.arialL, colRight(0), capBase, k, PanelAlign.RIGHT, DARK)
    metrics.forEachIndexed { c, m ->
        PanelText(if (m == 2) "WATTS" else unit(m), PanelFont.arialL, colRight(c + 1), capBase, k, PanelAlign.RIGHT, DARK)
    }
    shown.forEachIndexed { r, lap ->
        val top = rowTop + r * rowH
        val base = top + ((rowH + rf) / 2).toInt()
        PanelFill(0f, top, 540f, 1f, k)
        PanelText("${lap.number}", PanelFont.digits(rf), 24f, base, k)
        PanelText(lapText(lap.splitMs), PanelFont.digits(rf), colRight(0), base, k, PanelAlign.RIGHT)
        metrics.forEachIndexed { c, m ->
            PanelText(value(m, lap.speedKmh, lap.cadence, lap.powerW), PanelFont.digits(rf), colRight(c + 1), base, k, PanelAlign.RIGHT)
        }
    }
    if (p.rows == null) {
        PanelText(
            "Update the device's firmware to see its laps here", PanelFont.arialL, 270f, rowTop + 40, k,
            PanelAlign.CENTER, DARK,
        )
    }
}

// MARK: - WORKOUT

/** One workout as the device's WORKOUT page needs it. */
data class WorkoutPageModel(
    val loaded: Boolean = true,
    val name: String = "Sweet Spot 3x10.erg",
    val blockIdx: Int = 2,
    val blockCount: Int = 7,
    val running: Boolean = true,
    val paused: Boolean = false,
    val done: Boolean = false,
    val elapsed: Int = 23 * 60 + 48,
    val total: Int = 60 * 60,
    val targetW: Int = 230,
    val ftpW: Int = 250,
    /** The rider's power, null without a power meter. */
    val power: Int? = 238,
    /** The file's blocks; empty until the file has been read. */
    val blocks: List<Block> = listOf(
        Block(600, 140), Block(300, 175), Block(600, 230), Block(300, 150),
        Block(600, 230), Block(300, 150), Block(900, 130),
    ),
    /** The device's /workouts, shown as its picker when nothing is loaded. */
    val picker: List<String> = emptyList(),
    /** The workout file couldn't be read, so there is no block profile. */
    val fileUnavailable: Boolean = false,
) {
    data class Block(val seconds: Int, val watts: Int)

    /** Anything but a freshly loaded, untouched session. */
    val started: Boolean get() = running || paused || done || elapsed > 0

    val current: Block? get() = if (blocks.size == blockCount) blocks.getOrNull(blockIdx) else null
    val blockStart: Int get() = blocks.take(blockIdx).sumOf { it.seconds }

    /** Seconds left in the block: the session clock against the block's end. */
    val blockRemaining: Int?
        get() {
            val c = current ?: return null
            return (blockStart + c.seconds - elapsed).coerceIn(0, c.seconds)
        }

    /** workoutBlockTitle(): "Z4" and "THRESHOLD". */
    fun title(b: Block): Pair<String, String> {
        val z = zone(b.watts, ftpW)
        return (if (z > 0) "Z$z" else "--") to (if (z > 0) ZONE_NAMES[z - 1] else "BLOCK")
    }

    companion object {
        val ZONE_NAMES = listOf("RECOVERY", "ENDURANCE", "TEMPO", "THRESHOLD", "VO2 MAX", "ANAEROBIC", "NEUROMUSC")

        fun zone(watts: Int, ftp: Int): Int {
            if (ftp <= 0) return 0
            val pct = watts * 100 / ftp
            return when {
                pct <= 55 -> 1
                pct <= 75 -> 2
                pct <= 90 -> 3
                pct <= 105 -> 4
                pct <= 120 -> 5
                pct <= 150 -> 6
                else -> 7
            }
        }

        /**
         * The blocks of an .erg (minutes, watts) or .mrc (minutes, %FTP) file,
         * as the device reads them: one block between each pair of points, at
         * the average of its ends.
         */
        fun blocks(fromFile: String, ftp: Int): List<Block> {
            var percent = false
            var inData = false
            val pts = ArrayList<Pair<Double, Double>>()
            for (raw in fromFile.split("\n")) {
                val line = raw.trim()
                val upper = line.uppercase()
                if (!inData) {
                    if ("[COURSE DATA]" in upper) inData = true else if ("PERCENT" in upper) percent = true
                } else if ("[END COURSE DATA]" in upper) {
                    break
                } else {
                    val tok = line.split(' ', '\t').filter { it.isNotEmpty() }
                    val m = tok.getOrNull(0)?.toDoubleOrNull()
                    val v = tok.getOrNull(1)?.toDoubleOrNull()
                    if (m != null && v != null) pts.add(m to v)
                }
            }
            if (pts.size < 2) return emptyList()
            val out = ArrayList<Block>()
            for (i in 1 until pts.size) {
                val dur = (pts[i].first - pts[i - 1].first) * 60
                if (dur < 1) continue
                val avg = (pts[i - 1].second + pts[i].second) / 2
                val watts = if (percent) avg * maxOf(ftp, 1) / 100 else avg
                out.add(Block(dur.roundToInt(), watts.roundToInt()))
            }
            return out
        }

        /** "sweet_spot_3x10.erg" -> "sweet spot 3x10". */
        fun title(file: String): String {
            val stem = file.substringBeforeLast('.', file)
            return stem.ifEmpty { file }.replace('_', ' ')
        }
    }
}

/** What the WORKOUT page's controls do — the device's own taps (ui_dashboard.cpp). */
class WorkoutPageActions(
    /** PAUSE / START BLOCK: start, pause or resume. */
    val toggle: () -> Unit,
    /** A block strip or an ALL BLOCKS row: jump to that block. */
    val jump: (Int) -> Unit,
    /** READY's "< BACK · ALL WORKOUTS": unload, back to the picker. */
    val unload: () -> Unit,
    /** A picker row: load that file. */
    val load: (String) -> Unit,
    /** The picker's REFRESH. */
    val refresh: () -> Unit,
)

private const val W_ROW_TOP = 143f
private const val W_ROW_H = 88f
private const val W_ROWS = 8

/**
 * The device's WORKOUT page: ui_render_workout when a workout is loaded, the
 * workout picker (ui_render_workout_picker) when not.
 */
@Composable
fun WorkoutPagePreview(
    modifier: Modifier = Modifier,
    status: PanelStatus = PanelStatus.sample,
    model: WorkoutPageModel = WorkoutPageModel(),
    /** null = a static card (the editor); set = every control is live. */
    actions: WorkoutPageActions? = null,
) {
    var allOpen by remember { mutableStateOf(false) }
    var listPage by remember { mutableIntStateOf(0) }
    var pickPage by remember { mutableIntStateOf(0) }
    LaunchedEffect(model.loaded) { if (!model.loaded) allOpen = false }
    DevicePanel(modifier, status) { k ->
        when {
            !model.loaded -> WorkoutPicker(model, actions, k, pickPage) { pickPage = it }
            allOpen -> WorkoutBlockList(model, actions, k, listPage, { listPage = it }) { allOpen = false }
            else -> WorkoutLoaded(model, actions, k) {
                listPage = model.blockIdx / W_ROWS
                allOpen = true
            }
        }
    }
}

@Composable
private fun WorkoutLoaded(m: WorkoutPageModel, actions: WorkoutPageActions?, k: Float, onAll: () -> Unit) {
    val tol = 8
    val diff = m.power?.let { it - m.targetW } ?: 0
    val dir = when {
        m.power == null -> "NO POWER"
        diff < -tol -> "GO HARDER"
        diff > tol -> "EASE OFF"
        else -> "IN RANGE"
    }
    val cur = m.current
    val t = cur?.let { m.title(it) }
    val remain = m.blockRemaining
    PanelText(m.name, PanelFont.arialL, 24f, 92f, k, maxWidth = 492f)
    PanelText("BLOCK ${m.blockIdx + 1} OF ${m.blockCount}", PanelFont.arialB, 24f, 126f, k)
    PanelFill(0f, 140f, 540f, 3f, k)

    // The dark block: power, correction, target, the block's countdown.
    PanelFill(0f, 143f, 540f, 381f, k)
    PanelText("YOUR POWER · 3S", PanelFont.arialL, 24f, 188f, k, color = Color.White)
    PanelText(dir, PanelFont.arialL, 516f, 188f, k, PanelAlign.RIGHT, Color.White)
    val pw = m.power?.toString() ?: "--"
    PanelText(pw, PanelFont.impactH, 24f, 342f, k, color = Color.White)
    PanelText("W", PanelFont.arialB, 34f + PanelFont.impactH.width(pw), 342f, k, color = Color.White)
    if (m.power != null) {
        PanelText(
            "${if (diff < -tol || diff > tol) abs(diff) else 0}", PanelFont.impactH, 462f, 342f, k,
            PanelAlign.RIGHT, Color.White,
        )
        WorkoutArrow(diff, tol, k)
    }
    PanelText(
        "TARGET ${maxOf(0, m.targetW - tol)}-${m.targetW + tol} W · ${if (m.ftpW > 0) m.targetW * 100 / m.ftpW else 0}% FTP",
        PanelFont.arialL, 24f, 386f, k, color = Color.White,
    )
    if (cur != null && t != null) {
        PanelText("${t.first} ${t.second} · ${mmss(cur.seconds)} BLOCK", PanelFont.arialL, 24f, 412f, k, color = Color.White)
    }
    PanelRect(24f, 452f, 300f, 20f, k, line = 2f, edge = Color.White)
    if (cur != null && remain != null && cur.seconds > 0) {
        PanelFill(26f, 454f, 296f * (cur.seconds - remain) / cur.seconds, 16f, k, Color.White)
    }
    PanelText(
        when {
            m.done -> "DONE"
            m.paused -> "PAUSED"
            m.running -> "LEFT"
            else -> "READY"
        },
        PanelFont.arialL, 516f, 434f, k, PanelAlign.RIGHT, Color.White,
    )
    PanelText(remain?.let { mmss(it) } ?: "--:--", PanelFont.impactT, 516f, 496f, k, PanelAlign.RIGHT, Color.White)

    // Session profile: width is duration, height intensity; done hatched,
    // now solid, upcoming light.
    PanelText("SESSION PROFILE", PanelFont.arialL, 24f, 562f, k)
    WorkoutProfile(m, k)
    PanelText("0:00", PanelFont.arialL, 24f, 726f, k)
    PanelText(mmss(m.total), PanelFont.arialL, 516f, 726f, k, PanelAlign.RIGHT)
    PanelFill(0f, 744f, 540f, 3f, k)

    WorkoutStrips(m, k)

    PanelRect(24f, 868f, 240f, 80f, k)
    PanelRect(276f, 868f, 240f, 80f, k)
    PanelText(if (m.running) "PAUSE BLOCK" else "START BLOCK", PanelFont.arialB, 144f, 914f, k, PanelAlign.CENTER)
    PanelText("ALL BLOCKS", PanelFont.arialB, 396f, 914f, k, PanelAlign.CENTER)

    if (actions != null) {
        val ready = !m.started
        PanelTap(24f, 754f, 240f, 104f, k, if (ready) "Back to all workouts" else "Redo previous block") {
            if (ready) actions.unload() else actions.jump(maxOf(0, m.blockIdx - 1))
        }
        PanelTap(276f, 754f, 240f, 104f, k, "Start next block", enabled = m.blockCount > 0) {
            actions.jump(minOf(m.blockIdx + 1, maxOf(0, m.blockCount - 1)))
        }
        PanelTap(24f, 868f, 240f, 80f, k, if (m.running) "Pause block" else "Start block") { actions.toggle() }
        PanelTap(276f, 868f, 240f, 80f, k, "All blocks") { onAll() }
    }
}

@Composable
private fun WorkoutArrow(diff: Int, tol: Int, k: Float) {
    Canvas(Modifier.fillMaxSize()) {
        val s = k * density
        val ax = 494f
        val ay = 300f
        when {
            diff < -tol -> drawPath(
                Path().apply {
                    moveTo(ax * s, (ay - 26) * s)
                    lineTo((ax - 22) * s, (ay + 16) * s)
                    lineTo((ax + 22) * s, (ay + 16) * s)
                    close()
                },
                Color.White,
            )
            diff > tol -> drawPath(
                Path().apply {
                    moveTo(ax * s, (ay + 26) * s)
                    lineTo((ax - 22) * s, (ay - 16) * s)
                    lineTo((ax + 22) * s, (ay - 16) * s)
                    close()
                },
                Color.White,
            )
            else -> {
                drawRect(Color.White, Offset((ax - 20) * s, (ay - 14) * s), Size(40 * s, 9 * s))
                drawRect(Color.White, Offset((ax - 20) * s, (ay + 7) * s), Size(40 * s, 9 * s))
            }
        }
    }
}

@Composable
private fun WorkoutProfile(m: WorkoutPageModel, k: Float) {
    val cx0 = 24f
    val cw = 492f
    val cbase = 706f
    val ctop = 574f
    val blocks = m.blocks
    if (blocks.isEmpty()) {
        PanelText(
            if (m.fileUnavailable) "Block profile unavailable" else "Reading the workout file…",
            PanelFont.arialL, 270f, 650f, k, PanelAlign.CENTER, DARK,
        )
    }
    val total = maxOf(1, blocks.sumOf { it.seconds })
    val maxW = maxOf(1, blocks.maxOfOrNull { it.watts } ?: 1)
    val gaps = 2f * maxOf(0, blocks.size - 1)
    var x = cx0
    blocks.forEachIndexed { i, b ->
        var bw = (cw - gaps) * b.seconds / total
        if (i == blocks.lastIndex) bw = cx0 + cw - x
        bw = maxOf(4f, bw)
        val bh = 24f + 100f * b.watts / maxW
        Box(
            Modifier
                .offset((x * k).dp, ((cbase - bh) * k).dp)
                .size((bw * k).dp, (bh * k).dp)
                .then(
                    when {
                        i < m.blockIdx -> Modifier.panelHatch(k)
                        i == m.blockIdx -> Modifier.background(Color.Black)
                        else -> Modifier.background(Color.Black.copy(alpha = 0.15f))
                    },
                )
                .border(maxOf((if (i == m.blockIdx) 2f else 1f) * k, 0.5f).dp, Color.Black),
        )
        x += bw + 2
    }
    PanelFill(cx0, cbase - 3, cw, 3f, k)
    if (m.total > 0) {
        val mx = minOf(cx0 + cw * m.elapsed / m.total, cx0 + cw - 3)
        PanelFill(mx, ctop - 6, 3f, cbase - ctop + 6, k)
    }
}

/** Redo-previous and start-next strips. */
@Composable
private fun WorkoutStrips(m: WorkoutPageModel, k: Float) {
    val ready = !m.started
    PanelRect(24f, 754f, 240f, 104f, k, line = 2f)
    if (ready) {
        PanelText("< BACK", PanelFont.arialL, 38f, 780f, k)
        PanelText("ALL WORKOUTS", PanelFont.arialB, 38f, 812f, k)
        PanelText("PICK ANOTHER", PanelFont.arialL, 38f, 846f, k, color = DARK)
    } else {
        Box(Modifier.offset((26 * k).dp, (756 * k).dp).size((236 * k).dp, (100 * k).dp).panelHatch(k))
        if (m.blockIdx > 0 && m.blocks.size == m.blockCount) {
            val pb = m.blocks[m.blockIdx - 1]
            val t = m.title(pb)
            Chip("${pb.watts} W", PanelFont.impactT, 38f, 848f, k)
            Chip(t.second, PanelFont.arialB, 38f, 812f, k)
            Chip("< REDO · ${t.first}", PanelFont.arialL, 38f, 780f, k)
        } else {
            Chip("< REDO", PanelFont.arialL, 38f, 780f, k)
            Chip(if (m.blockIdx > 0) "PREVIOUS" else "SESSION START", PanelFont.arialB, 38f, 812f, k)
        }
    }
    PanelRect(276f, 754f, 240f, 104f, k, line = 2f)
    if (m.blockIdx + 1 < m.blockCount) {
        if (m.blocks.size == m.blockCount) {
            val nb = m.blocks[m.blockIdx + 1]
            val t = m.title(nb)
            PanelText("START NEXT · ${t.first} >", PanelFont.arialL, 290f, 780f, k)
            PanelText("${t.second} · ${mmss(nb.seconds)}", PanelFont.arialB, 290f, 812f, k, maxWidth = 216f)
            PanelText("${nb.watts} W", PanelFont.impactT, 290f, 848f, k)
        } else {
            PanelText("START NEXT >", PanelFont.arialL, 290f, 780f, k)
        }
    } else {
        PanelText("NEXT >", PanelFont.arialL, 290f, 780f, k)
        PanelText("SESSION COMPLETE", PanelFont.arialB, 290f, 812f, k, maxWidth = 216f)
    }
}

/** Text on a paper chip over the hatched strip, so it stays readable. */
@Composable
private fun Chip(s: String, f: PanelFont, x: Float, baseline: Float, k: Float) {
    val capH = f.size * 0.72f
    PanelFill(x - 3, baseline - capH - 3, f.width(s) + 6, capH + 8, k, PanelPaper)
    PanelText(s, f, x, baseline, k)
}

/** ALL BLOCKS (ui_render_workout_list). */
@Composable
private fun WorkoutBlockList(
    m: WorkoutPageModel,
    actions: WorkoutPageActions?,
    k: Float,
    page: Int,
    setPage: (Int) -> Unit,
    onClose: () -> Unit,
) {
    val first = page * W_ROWS
    val rows = (maxOf(0, first) until minOf(m.blockCount, first + W_ROWS)).toList()
    val total = m.blocks.sumOf { it.seconds }
    PanelText("START A BLOCK", PanelFont.arialL, 24f, 92f, k)
    PanelText("${m.name} · ${mmss(if (total > 0) total else m.total)}", PanelFont.impactT, 24f, 134f, k, maxWidth = 492f)
    PanelFill(0f, 140f, 540f, 3f, k)
    for (i in rows) {
        val top = W_ROW_TOP + (i - first) * W_ROW_H
        val done = i < m.blockIdx
        val now = i == m.blockIdx
        val x = if (now) 16f else 24f
        PanelFill(0f, top + W_ROW_H - 1, 540f, 1f, k)
        if (now) PanelFill(0f, top, 8f, W_ROW_H, k)
        Box(
            Modifier
                .offset((x * k).dp, ((top + 18) * k).dp)
                .size((10 * k).dp, (52 * k).dp)
                .then(
                    when {
                        done -> Modifier.panelHatch(k)
                        now -> Modifier.background(Color.Black)
                        else -> Modifier
                    },
                )
                .border(maxOf(2 * k, 0.6f).dp, Color.Black),
        )
        if (m.blocks.size == m.blockCount) {
            val b = m.blocks[i]
            val t = m.title(b)
            PanelText(t.first, PanelFont.arialB, x + 24, top + 52, k)
            PanelText(t.second, PanelFont.arialB, x + 82, top + 40, k)
            PanelText(mmss(b.seconds), PanelFont.impactT, 396f, top + 58, k, PanelAlign.RIGHT)
            PanelText("${b.watts}W", PanelFont.impactT, 516f, top + 58, k, PanelAlign.RIGHT)
        } else {
            PanelText("BLOCK ${i + 1}", PanelFont.arialB, x + 82, top + 40, k)
        }
        PanelText(
            if (done) "DONE" else if (now) "RUNNING NOW" else "TAP TO START",
            PanelFont.arialL, x + 82, top + 66, k, color = DARK,
        )
        if (actions != null) {
            PanelTap(0f, top, 540f, W_ROW_H, k, "Start block ${i + 1}") {
                actions.jump(i)
                onClose()
            }
        }
    }
    PanelPager(
        first, rows.size, m.blockCount, "CLOSE", k,
        onUp = actions?.let { { if (page > 0) setPage(page - 1) } },
        onDown = actions?.let { { if ((page + 1) * W_ROWS < m.blockCount) setPage(page + 1) } },
        onAction = actions?.let { { onClose() } },
    )
}

@Composable
private fun WorkoutPicker(
    m: WorkoutPageModel,
    actions: WorkoutPageActions?,
    k: Float,
    page: Int,
    setPage: (Int) -> Unit,
) {
    val list = m.picker
    val first = minOf(page * W_ROWS, maxOf(0, list.size - 1))
    val rows = list.withIndex().drop(first).take(W_ROWS)
    if (list.isEmpty()) {
        PanelText("NO WORKOUTS ON THE CARD", PanelFont.impactT, 270f, 400f, k, PanelAlign.CENTER)
        PanelText("Add them from the app.", PanelFont.arialB, 270f, 456f, k, PanelAlign.CENTER, DARK)
        PanelText("(.erg / .mrc files in /workouts)", PanelFont.arialB, 270f, 492f, k, PanelAlign.CENTER, DARK)
    } else {
        PanelText("TAP A WORKOUT TO LOAD IT", PanelFont.arialL, 24f, 92f, k)
        PanelText("${list.size} WORKOUT${if (list.size == 1) "" else "S"}", PanelFont.impactT, 24f, 134f, k)
        PanelFill(0f, 140f, 540f, 3f, k)
        for ((i, name) in rows) {
            val top = W_ROW_TOP + (i - first) * W_ROW_H
            PanelText(WorkoutPageModel.title(name), PanelFont.arialB, 24f, top + 34, k, maxWidth = 492f)
            PanelText(name, PanelFont.arialL, 24f, top + 70, k, color = DARK, maxWidth = 492f)
            PanelFill(0f, top + W_ROW_H - 1, 540f, 1f, k)
            if (actions != null) {
                PanelTap(0f, top, 540f, W_ROW_H, k, "Load workout ${WorkoutPageModel.title(name)}") { actions.load(name) }
            }
        }
    }
    PanelPager(
        if (list.isEmpty()) 0 else first, rows.size, list.size, "REFRESH", k,
        onUp = actions?.let { { if (page > 0) setPage(page - 1) } },
        onDown = actions?.let { { if ((page + 1) * W_ROWS < list.size) setPage(page + 1) } },
        onAction = actions?.let { a -> { a.refresh() } },
    )
}
