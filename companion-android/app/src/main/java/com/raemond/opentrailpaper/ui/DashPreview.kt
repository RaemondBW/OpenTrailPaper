package com.raemond.opentrailpaper.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material3.LocalTextStyle
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.data.DashField
import com.raemond.opentrailpaper.data.DashItem
import com.raemond.opentrailpaper.data.DashLayout
import com.raemond.opentrailpaper.data.DashSize
import com.raemond.opentrailpaper.data.RideSim

// A scaled likeness of the panel, drawn to the SAME rules ui_render.cpp uses.
//
// The rider is choosing a layout against this picture, so anywhere it differs
// from the panel it is actively misleading. It kept drifting on iOS because it
// was an impression of the device rather than a port of it — a value size guessed
// per cell from a character count, no hero treatment at all, no zone bar.
//
// So it runs the firmware's actual algorithm, in DEVICE PIXELS, and scales the
// result once at the end:
//   - the same type ladders, with the real font metrics baked in below;
//   - one value face per (size class, cell width), the smallest any cell of that
//     class needs, which is what stops two same-sized cells disagreeing;
//   - one caption face for the whole screen;
//   - the hero on its own ladder, caption centred, with the FTP zone bar.

/**
 * One face from the firmware's ladders. Sizes are device pixels: [cap] is the
 * digit height ui_render measures against, and the advances are what its
 * textWidth() returns. Printed straight out of the host build's font tables —
 * guessing these is how the preview drifted last time.
 */
private class Face(val cap: Float, val w8: Float, val w1: Float, val wDot: Float, val wColon: Float) {
    fun width(s: String): Float = s.sumOf {
        when (it) {
            '.' -> wDot.toDouble()
            ':' -> wColon.toDouble()
            '1' -> w1.toDouble()
            else -> w8.toDouble()
        }
    }.toFloat()
}

/** kValueLadder, strictly descending — the firmware relies on that order and so
 *  does the "smallest face any cell needs" pass below. */
private val VALUE_LADDER = listOf(
    Face(158f, 103f, 73f, 35f, 39f),   // Impact_XL
    Face(120f, 78f, 56f, 27f, 30f),    // Impact_C
    Face(95f, 61f, 44f, 21f, 23f),     // Impact_H
    Face(81f, 53f, 38f, 18f, 20f),     // Impact_B
    Face(69f, 45f, 32f, 16f, 17f),     // Impact_M
    Face(58f, 38f, 27f, 13f, 14f),     // Impact_A
    Face(46f, 30f, 21f, 10f, 11f),     // Impact_V
    Face(30f, 20f, 14f, 7f, 8f),       // Impact_T
    Face(15f, 12f, 12f, 6f, 7f),       // Arial_B
)

/** The hero steps through its own ladder: XL, H, M, V. */
private val HERO_LADDER =
    listOf(VALUE_LADDER[0], VALUE_LADDER[2], VALUE_LADDER[4], VALUE_LADDER[6])

/** kLabelLadder. [perChar] is the tracked caption's average advance, taken from
 *  the width of "HEART RATE" in each face. */
private class LabelFace(val cap: Float, val ascender: Float, val perChar: Float)

private val LABEL_LADDER = listOf(
    LabelFace(28f, 38f, 30.1f),   // ArialBold_20
    LabelFace(15f, 19f, 16.6f),   // Arial_B
    LabelFace(11f, 14f, 12.4f),   // Arial_L
)

/** Arial_B, the face the unit caption is set in. */
private val UNIT_FACE = LabelFace(15f, 19f, 13.5f)

// Device geometry (ui_render.h). Everything is computed in these units.
private const val PANEL_W = 540f
private const val PANEL_H = 960f
private const val MARGIN = 24f
private const val GUTTER = 12f
private const val PAD = 16f
private const val STATUS_H = 64f
private const val STEP = 12f
private const val HALF_STEP = 6f
private const val RULE = 2f
private const val CONTENT_W = PANEL_W - 2 * MARGIN
private const val BODY_H = PANEL_H - STATUS_H

/** Barlow's cap height is 0.72 em, so this converts a device cap height into the
 *  point size that draws the same-sized capital. */
private const val CAP_RATIO = 0.72f

/** The panel's own paper, a shade lighter than the app's. */
val PanelPaper = Color(0xFFF7F5EF)

/** Width : height of the area [DashPreview] draws — the panel minus its status
 *  bar. Callers apply it so the preview is always the shape of the real screen. */
const val DASH_PANEL_ASPECT = PANEL_W / BODY_H

private class Placed(
    val x: Float,
    val y: Float,
    val w: Float,
    val h: Float,
    val item: DashItem,
    val hero: Boolean,
    var value: Face,
    var label: LabelFace,
)

/**
 * @param live a ride to read the numbers off, for the tutorial's head unit. The
 * editor leaves it null and keeps the fixed samples: someone choosing a layout is
 * comparing box sizes, and numbers changing under them is noise.
 *
 * Only the DRAWN text comes from [live]. Type sizing still runs off the samples
 * and the worst-case hints below, so a live panel picks exactly the faces the
 * editor showed and no value can resize its own cell mid-ride — which is the
 * device's behaviour, and the whole reason this sizes in two passes.
 */
@Composable
fun DashPreview(
    layout: DashLayout,
    modifier: Modifier = Modifier,
    live: RideSim? = null,
) {
    BoxWithConstraints(
        modifier
            .background(PanelPaper)
            .clipToBounds(),
    ) {
        val k = maxWidth.value / PANEL_W
        val density = LocalDensity.current
        // Panel geometry is absolute: a phone-wide font scale would resize the
        // numbers without resizing their cells, which is precisely the mismatch
        // this preview exists to rule out.
        CompositionLocalProvider(
            LocalDensity provides Density(density.density, fontScale = 1f),
            LocalTextStyle provides LocalTextStyle.current.copy(color = Color.Black),
        ) {
            for (p in place(layout)) {
                Box(
                    Modifier
                        .offset(x = (p.x * k).dp, y = ((p.y - STATUS_H) * k).dp)
                        .size(width = (p.w * k).dp, height = (p.h * k).dp),
                ) {
                    Cell(p, k, live)
                }
            }
        }
    }
}

@Composable
private fun Cell(p: Placed, k: Float, live: RideSim?) {
    Box(
        Modifier
            .fillMaxSize()
            .border((RULE * k).dp, Color.Black),
    ) {
        if (p.hero) {
            Column(
                Modifier
                    .fillMaxSize()
                    .padding(horizontal = (PAD * k).dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Caption(p, k, Modifier.padding(top = (PAD * k).dp).fillMaxWidth(), centre = true)
                Box(Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.Center) {
                    Value(p, k, live)
                }
                if (isPower(p.item.field)) {
                    ZoneBar(k, p.w - 2 * PAD, filled = zone(p.item.field, live))
                    Spacer(Modifier.height((PAD * k).dp))
                }
            }
        } else {
            Caption(p, k, Modifier.padding(start = (PAD * k).dp, top = (PAD * k).dp))
            Box(
                Modifier
                    .fillMaxSize()
                    .padding(top = ((PAD + p.label.ascender) * k).dp),
                contentAlignment = Alignment.Center,
            ) {
                Value(p, k, live)
            }
        }
    }
}

@Composable
private fun Caption(p: Placed, k: Float, modifier: Modifier = Modifier, centre: Boolean = false) {
    Text(
        p.item.panelLabel,
        modifier = modifier,
        style = barlow((p.label.cap / CAP_RATIO * k).sp, FontWeight.SemiBold)
            .copy(letterSpacing = (p.label.cap * 0.18f * k).sp),
        maxLines = 1,
        textAlign = if (centre) TextAlign.Center else TextAlign.Start,
    )
}

/**
 * Value and unit are ONE object, centred as a pair — centring the number alone
 * pushes it off-axis by half the unit's width.
 */
@Composable
private fun Value(p: Placed, k: Float, live: RideSim?) {
    Row(
        verticalAlignment = Alignment.Bottom,
        horizontalArrangement = Arrangement.spacedBy((5 * k).dp),
    ) {
        Text(
            live?.text(p.item.field) ?: sample(p.item.field),
            style = condensed((p.value.cap / CAP_RATIO * k).sp, FontWeight.Bold),
            maxLines = 1,
        )
        unitFor(p.item.field)?.let {
            Text(
                it,
                style = barlow((UNIT_FACE.cap / CAP_RATIO * k).sp, FontWeight.SemiBold),
                maxLines = 1,
            )
        }
    }
}

/** Seven FTP zone segments, filled to the current zone. */
@Composable
private fun ZoneBar(k: Float, width: Float, filled: Int) {
    val segW = (width - 6 * HALF_STEP) / 7
    Row(horizontalArrangement = Arrangement.spacedBy((HALF_STEP * k).dp)) {
        for (i in 0 until 7) {
            Box(
                Modifier
                    .size(width = (segW * k).dp, height = (18 * k).dp)
                    .background(if (i < filled) Color.Black else Color.Transparent)
                    .border((1 * k).dp, Color.Black),
            )
        }
    }
}

// MARK: layout — ui_render.cpp's packer, verbatim in device pixels

private fun place(layout: DashLayout): List<Placed> {
    val rows = layout.rows
    if (rows.isEmpty()) return emptyList()
    val weights = rows.map { row -> row.maxOfOrNull { it.size.weight } ?: 1 }
    val total = maxOf(weights.sum(), 1)
    val gutters = (rows.size - 1) * GUTTER
    val availH = (PANEL_H - STATUS_H - MARGIN) - gutters
    val halfW = (CONTENT_W - GUTTER) / 2

    val out = ArrayList<Placed>()
    var y = STATUS_H + MARGIN - STEP
    for ((r, row) in rows.withIndex()) {
        val rowH = if (r == rows.size - 1) {
            PANEL_H - MARGIN - y
        } else {
            availH * weights[r] / total
        }
        for ((c, item) in row.withIndex()) {
            val w = if (row.size == 2) halfW else CONTENT_W
            val x = if (row.size == 2) {
                if (c == 0) MARGIN else MARGIN + halfW + GUTTER
            } else {
                MARGIN
            }
            // The hero only gets hero treatment when it is alone on a row with
            // the height to carry it, exactly as the device decides.
            val hero = item.size == DashSize.HERO && row.size == 1 && rowH >= 200
            out.add(
                Placed(
                    x, y, w, rowH, item, hero,
                    VALUE_LADDER.last(),
                    LABEL_LADDER.last(),
                ),
            )
        }
        y += rowH + GUTTER
    }
    return sized(out)
}

/**
 * Second pass: equalise type across each size class, as the device does. A cell
 * that sizes itself independently makes identical boxes disagree.
 */
private fun sized(out: List<Placed>): List<Placed> {
    val classIdx = HashMap<String, Int>()
    var labelIdx = 0

    for (p in out) {
        if (p.hero) continue
        val availW = p.w - 2 * PAD
        val unitW = unitFor(p.item.field)?.let { UNIT_FACE.perChar * it.length + 6 } ?: 0f
        val valH = p.h - PAD * 2 - LABEL_LADDER[1].ascender - HALF_STEP
        val idx = valueFaceIndex(VALUE_LADDER, hint(p.item.field), availW, valH, unitW)
        val key = "${p.item.size.token}-${bucket(p.w)}"
        classIdx[key] = maxOf(classIdx[key] ?: 0, idx)

        val li = LABEL_LADDER.indexOfFirst { it.perChar * p.item.panelLabel.length <= availW }
        labelIdx = maxOf(labelIdx, if (li >= 0) li else LABEL_LADDER.size - 1)
    }

    for (p in out) {
        p.label = LABEL_LADDER[labelIdx]
        p.value = if (p.hero) {
            heroFace(p)
        } else {
            VALUE_LADDER[classIdx["${p.item.size.token}-${bucket(p.w)}"] ?: 0]
        }
    }
    return out
}

private fun bucket(w: Float) = if (w > CONTENT_W * 3 / 4) "wide" else "narrow"

private fun valueFaceIndex(
    ladder: List<Face>,
    text: String,
    availW: Float,
    availH: Float,
    unitW: Float,
): Int {
    val i = ladder.indexOfFirst { it.cap <= availH && it.width(text) + unitW <= availW }
    return if (i >= 0) i else ladder.size - 1
}

private fun heroFace(p: Placed): Face {
    val innerW = p.w - 2 * PAD
    val unitW = unitFor(p.item.field)?.let { UNIT_FACE.perChar * it.length + 10 } ?: 0f
    val barH = if (isPower(p.item.field)) 18 + STEP else 0f
    val top = PAD + LABEL_LADDER[1].ascender + HALF_STEP
    val bot = p.h - PAD - barH
    return HERO_LADDER.firstOrNull {
        it.width(sample(p.item.field)) + unitW <= innerW && it.cap <= bot - top
    } ?: HERO_LADDER.last()
}

// MARK: field data

private fun isPower(f: String) = f == "power3s" || f == "power"

private fun unitFor(f: String): String? = when (f) {
    "speed" -> "KM/H"
    "power3s", "power" -> "W"
    "hr" -> "BPM"
    "cadence" -> "RPM"
    "distance", "routeleft" -> "KM"
    "climb", "altitude" -> "M"
    "grade", "battery" -> "%"
    else -> null
}

/**
 * dashSizingHint(): the WIDEST string a field can produce. The device sizes type
 * for the worst case so the number never resizes mid-ride, and the preview has to
 * size for the same string or it shows the wrong face.
 */
private fun hint(f: String): String = when (f) {
    "speed", "grade" -> "88.8"
    "distance", "routeleft" -> "888.8"
    "ridetime", "movingtime" -> "88:88:88"
    "climb", "altitude" -> "8888"
    "sats" -> "88"
    "clock" -> "88:88"
    else -> "888"
}

/** Plausible live values, so the preview reads like a ride in progress. */
private fun sample(field: String): String = when (field) {
    "speed" -> "32.4"
    "power3s", "power" -> "247"
    "hr" -> "156"
    "cadence" -> "88"
    "distance" -> "54.8"
    "ridetime", "movingtime" -> "1:47:12"
    "climb" -> "918"
    "grade" -> "4.2"
    "altitude" -> "112"
    "battery" -> "76"
    "sats" -> "11"
    "clock" -> "14:25"
    "routeleft" -> "12.4"
    else -> "--"
}

/**
 * Which zone the power field is in, against the device's default 250 W FTP —
 * Coggan's boundaries, the ones ui_render.cpp fills the bar by. The fixed sample
 * of 247 W sits at 99% of FTP, which is zone 4, so a static preview looks exactly
 * as it always did.
 */
private fun zone(field: String, live: RideSim?): Int {
    val watts = live?.let { if (field == "power") it.power else it.power3s } ?: 247.0
    val pct = watts / 250 * 100
    return when {
        pct < 55 -> 1
        pct < 75 -> 2
        pct < 90 -> 3
        pct < 105 -> 4
        pct < 120 -> 5
        pct < 150 -> 6
        else -> 7
    }
}

// MARK: - page thumbnails

// Thumbnails of the device's MUSIC and MAP screens, same idiom as DashPreview:
// device geometry (540x960) scaled by width, drawn with the same content the
// panel shows so the carousel cards are honest previews. Ports of MusicPreview
// and MapPagePreview in companion-ios/Sources/DashboardEditorView.swift.

private fun previewPanelLabel(id: String): String =
    if (id == "power3s") "POWER · 3S"
    else (DashField.named(id)?.label ?: id).uppercase()

private fun previewSample(id: String): String =
    if (id == "ridetime" || id == "movingtime") "1:47" else sample(id)

@Composable
fun MusicPreview(modifier: Modifier = Modifier) {
    PanelMock(modifier) { k ->
        // Status band: clock, title, battery — as the device draws it.
        PanelText("14:25", 30f * k, FontWeight.Bold, x = 14f * k, y = 14f * k)
        PanelText(
            "MUSIC", 26f * k, FontWeight.Black,
            x = 0f, y = 16f * k, width = 540f * k, align = TextAlign.Center,
            tracking = 6f * k,
        )
        PanelText("76%", 30f * k, FontWeight.Bold, x = 440f * k, y = 14f * k)

        Canvas(Modifier.fillMaxSize()) {
            val s = k * density
            fun rect(x: Float, y: Float, w: Float, h: Float, fill: Boolean, stroke: Float = 2f) {
                if (fill) {
                    drawRect(Color.Black, Offset(x * s, y * s), Size(w * s, h * s))
                } else {
                    drawRect(
                        Color.Black, Offset(x * s, y * s), Size(w * s, h * s),
                        style = Stroke((stroke * s).coerceAtLeast(1f)),
                    )
                }
            }
            // Status rule
            rect(0f, 61f, 540f, 3f, fill = true)
            // Album art frame + vinyl placeholder
            rect(108f, 96f, 324f, 324f, fill = false)
            drawCircle(
                Color.Black, 104f * s, Offset(270f * s, 258f * s),
                style = Stroke((2f * s).coerceAtLeast(1f)),
            )
            drawCircle(Color.Black, 34f * s, Offset(270f * s, 258f * s))
            drawCircle(PanelPaper, 12f * s, Offset(270f * s, 258f * s))
            // Volume stepper boxes with their + / - glyphs
            rect(462f, 140f, 76f, 76f, fill = false, stroke = 3f)
            rect(462f, 232f, 76f, 76f, fill = false, stroke = 3f)
            rect(486f, 175f, 28f, 6f, fill = true)
            rect(497f, 164f, 6f, 28f, fill = true)
            rect(486f, 267f, 28f, 6f, fill = true)
            // Progress + fill
            rect(24f, 640f, 492f, 14f, fill = false)
            rect(26f, 642f, 228f, 10f, fill = true)
            // Transport boxes
            rect(24f, 780f, 140f, 120f, fill = false)
            rect(176f, 780f, 188f, 120f, fill = false)
            rect(376f, 780f, 140f, 120f, fill = false)
            // prev: triangle to a bar; pause: two bars; next mirrored
            fun tri(cx: Float, cy: Float, w: Float, h: Float, left: Boolean) {
                val p = Path().apply {
                    if (left) {
                        moveTo((cx + w / 2) * s, (cy - h / 2) * s)
                        lineTo((cx + w / 2) * s, (cy + h / 2) * s)
                        lineTo((cx - w / 2) * s, cy * s)
                    } else {
                        moveTo((cx - w / 2) * s, (cy - h / 2) * s)
                        lineTo((cx - w / 2) * s, (cy + h / 2) * s)
                        lineTo((cx + w / 2) * s, cy * s)
                    }
                    close()
                }
                drawPath(p, Color.Black)
            }
            tri(88f, 840f, 26f, 32f, left = true)
            rect(66f, 824f, 6f, 32f, fill = true)
            rect(254f, 822f, 12f, 36f, fill = true)
            rect(274f, 822f, 12f, 36f, fill = true)
            tri(432f, 840f, 26f, 32f, left = false)
            rect(448f, 824f, 6f, 32f, fill = true)
        }

        PanelText(
            "VOL", 20f * k, FontWeight.Bold,
            x = 462f * k, y = 106f * k, width = 76f * k, align = TextAlign.Center,
            tracking = 4f * k,
        )
        PanelText(
            "TURN YOUR LIGHTS DOWN LOW", 34f * k, FontWeight.Black,
            x = 24f * k, y = 470f * k, width = 492f * k,
        )
        PanelText(
            "Bob Marley & The Wailers", 24f * k, FontWeight.SemiBold,
            x = 24f * k, y = 530f * k, width = 492f * k,
        )
        PanelText(
            "Exodus", 20f * k, FontWeight.Normal,
            x = 24f * k, y = 570f * k, width = 492f * k, faded = true,
        )
        PanelText("2:34", 20f * k, FontWeight.Normal, x = 24f * k, y = 668f * k)
        PanelText("5:31", 20f * k, FontWeight.Normal, x = 470f * k, y = 668f * k)
    }
}

@Composable
fun MapPagePreview(fields: List<String>, modifier: Modifier = Modifier) {
    PanelMock(modifier) { k ->
        Canvas(Modifier.fillMaxSize()) {
            val s = k * density
            // Status rule, then the street grid clipped to the map band.
            drawRect(Color.Black, Offset(0f, 61f * s), Size(540f * s, 3f * s))
            clipRect(0f, 64f * s, 540f * s, 810f * s) {
                rotate(18f, pivot = Offset(270f * s, 430f * s)) {
                    for (i in 0 until 4) {
                        drawRect(
                            Color.Black.copy(alpha = if (i == 1) 0.9f else 0.35f),
                            Offset(-80f * s, (150f + i * 150f) * s),
                            Size(700f * s, (if (i == 1) 7f else 3f) * s),
                        )
                    }
                    for (i in 0 until 3) {
                        drawRect(
                            Color.Black.copy(alpha = 0.35f),
                            Offset((120f + i * 160f) * s, 80f * s),
                            Size(3f * s, 700f * s),
                        )
                    }
                }
                // Position dot
                drawCircle(Color.Black, 13f * s, Offset(270f * s, 433f * s))
                drawCircle(
                    Color.Black, 22f * s, Offset(270f * s, 433f * s),
                    style = Stroke((2f * s).coerceAtLeast(1f)),
                )
                // Zoom stack
                for (v in 0 until 2) {
                    drawRect(
                        PanelPaper, Offset(462f * s, (560f + v * 80f) * s),
                        Size(76f * s, 76f * s),
                    )
                    drawRect(
                        Color.Black, Offset(462f * s, (560f + v * 80f) * s),
                        Size(76f * s, 76f * s), style = Stroke((3f * s).coerceAtLeast(1f)),
                    )
                    drawRect(
                        Color.Black, Offset(486f * s, (595f + v * 80f) * s),
                        Size(28f * s, 6f * s),
                    )
                    if (v == 0) {
                        drawRect(Color.Black, Offset(497f * s, 584f * s), Size(6f * s, 28f * s))
                    }
                }
            }
            // Data strip rule + cell boxes, boxed like the data pages' cells.
            drawRect(Color.Black, Offset(0f, 810f * s), Size(540f * s, 3f * s))
            for (c in 0 until 3) {
                drawRect(
                    Color.Black, Offset((24f + c * 168f) * s, 825f * s),
                    Size(156f * s, 123f * s), style = Stroke((2f * s).coerceAtLeast(1f)),
                )
            }
        }
        // The CONFIGURED fields, drawn like dash cells.
        for (c in 0 until 3) {
            val id = fields.getOrElse(c) { "speed" }
            PanelText(
                previewPanelLabel(id), 15f * k, FontWeight.Bold,
                x = (38f + c * 168f) * k, y = 837f * k, width = 130f * k,
                tracking = 2f * k,
            )
            PanelText(
                previewSample(id), 48f * k, FontWeight.Black,
                x = (24f + c * 168f) * k, y = 862f * k, width = 156f * k,
                align = TextAlign.Center,
            )
        }
    }
}

/** Shared scaffold: paper ground, 540x960 device space, font scale pinned. */
@Composable
private fun PanelMock(modifier: Modifier, content: @Composable (k: Float) -> Unit) {
    BoxWithConstraints(
        modifier
            .background(PanelPaper)
            .clipToBounds(),
    ) {
        val k = maxWidth.value / PANEL_W
        val density = LocalDensity.current
        CompositionLocalProvider(
            LocalDensity provides Density(density.density, fontScale = 1f),
            LocalTextStyle provides LocalTextStyle.current.copy(color = Color.Black),
        ) {
            content(k)
        }
    }
}

@Composable
private fun PanelText(
    text: String,
    sizeDp: Float,
    weight: FontWeight,
    x: Float,
    y: Float,
    width: Float? = null,
    align: TextAlign = TextAlign.Start,
    tracking: Float = 0f,
    faded: Boolean = false,
) {
    Text(
        text,
        fontSize = sizeDp.sp,
        fontWeight = weight,
        letterSpacing = tracking.sp,
        textAlign = align,
        maxLines = 1,
        overflow = TextOverflow.Ellipsis,
        color = if (faded) Color.Black.copy(alpha = 0.6f) else Color.Black,
        modifier = Modifier
            .offset(x = x.dp, y = y.dp)
            .then(if (width != null) Modifier.width(width.dp) else Modifier),
    )
}
