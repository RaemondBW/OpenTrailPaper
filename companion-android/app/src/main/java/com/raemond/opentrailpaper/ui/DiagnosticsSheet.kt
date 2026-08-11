package com.raemond.opentrailpaper.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.horizontalScroll
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
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Share
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.io.File
import java.util.Locale
import kotlin.math.abs

/**
 * Shows a downloaded device diagnostics log: a battery drain chart parsed from
 * the "battery:" lines, and the raw log text. Shareable.
 */
@Composable
fun DiagnosticsSheet(file: File, onDismiss: () -> Unit) {
    val context = LocalContext.current
    var tab by remember { mutableStateOf(0) }
    val text = remember(file) { runCatching { file.readText() }.getOrDefault("") }
    val samples = remember(text) { BatterySample.parse(text) }

    FullScreenSheet(
        title = "Diagnostics",
        onDismiss = onDismiss,
        trailing = {
            IconButton(onClick = { Share.log(context, file) }) {
                Icon(Icons.Filled.Share, contentDescription = "Share log", tint = Palette.accent)
            }
        },
    ) {
        Box(Modifier.padding(16.dp)) {
            Segmented(listOf("Battery", "Log"), tab) { tab = it }
        }
        if (tab == 0) BatteryTab(samples) else LogTab(text)
    }
}

@Composable
private fun BatteryTab(samples: List<BatterySample>) {
    var selPercent by remember { mutableStateOf<Int?>(null) }
    var selDraw by remember { mutableStateOf<Int?>(null) }
    val layout = remember(samples) { layoutSamples(samples) }

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        if (samples.size < 2) {
            Card {
                Text(
                    "No battery data yet. Let the device run for a while (it logs every 2 min " +
                        "while awake), then download the log again.",
                    style = TypeScale.body,
                    color = Palette.muted,
                )
            }
            return@Column
        }

        BatterySummary(samples)

        Card {
            TrackedLabel("Battery %")
            Text(
                "Tap the chart to read a value · shaded = unit off (time compressed)",
                style = barlow(11.sp),
                color = Palette.muted,
            )
            Spacer(Modifier.size(8.dp))
            BatteryChart(
                layout = layout,
                value = { it.percent },
                color = Palette.accent,
                yDomain = 0.0 to 100.0,
                selected = selPercent,
                onSelect = { selPercent = it },
                format = { String.format(Locale.US, "%.0f%%", it) },
                modifier = Modifier.fillMaxWidth().height(220.dp),
            )
        }

        Card {
            TrackedLabel("Current draw (mA)")
            Text(
                "Higher = drawing more · tap to read a value",
                style = barlow(11.sp),
                color = Palette.muted,
            )
            Spacer(Modifier.size(8.dp))
            BatteryChart(
                layout = layout,
                value = { it.drawMa },
                color = Palette.good,
                yDomain = null,
                selected = selDraw,
                onSelect = { selDraw = it },
                format = { String.format(Locale.US, "%.0f mA", it) },
                modifier = Modifier.fillMaxWidth().height(180.dp),
            )
        }
    }
}

@Composable
private fun BatterySummary(samples: List<BatterySample>) {
    val last = samples.last()
    val stats = remember(samples) { drainStats(samples) }
    Card {
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(16.dp)) {
            StatCell(String.format(Locale.US, "%.0f%%", last.percent), "Now", Modifier.weight(1f))
            if (stats != null) {
                StatCell(
                    String.format(Locale.US, "%.1f %%/hr", stats.first),
                    "Drain",
                    Modifier.weight(1f),
                )
                StatCell(
                    String.format(Locale.US, "%.0f mA", stats.second),
                    "Avg draw",
                    Modifier.weight(1f),
                )
                StatCell(
                    String.format(Locale.US, "%.1f h", last.percent / stats.first),
                    "Est. left",
                    Modifier.weight(1f),
                )
            } else {
                // Charging throughout, or too little discharging time to say.
                StatCell("—", "Drain", Modifier.weight(1f))
                StatCell("—", "Est. left", Modifier.weight(1f))
            }
        }
    }
}

@Composable
private fun StatCell(value: String, label: String, modifier: Modifier = Modifier) {
    Column(modifier) {
        Text(value, style = barlow(16.sp, FontWeight.Bold), color = Palette.ink, maxLines = 1)
        Text(label, style = barlow(11.sp), color = Palette.muted, maxLines = 1)
    }
}

/**
 * A point on the compressed timeline: `x` is the plotted position (off periods
 * collapsed), `session` groups contiguous awake runs so the line breaks across an
 * off period instead of drawing fake drain through it.
 */
private class PlotPoint(
    val x: Double,
    val percent: Double,
    val currentMa: Double,
    val session: Int,
    val timeLabel: String,
) {
    /**
     * The gauge reports current signed from the battery's point of view, so
     * discharging is negative (-174 mA) and it never goes positive in practice.
     * Plotting that put the whole "current draw" trace below zero, which reads as
     * though the device were producing power. Flip it so draw is a positive
     * number; a genuine charge current shows as negative.
     */
    val drawMa: Double get() = -currentMa
}

private class ChartLayout(
    val points: List<PlotPoint>,
    val marks: List<Pair<Double, String>>,
    val offBands: List<Pair<Double, Double>>,
)

/**
 * Lay the samples out on a compressed x-axis: the device logs every ~2 min while
 * awake, so a gap bigger than that means it was off — collapse those gaps to a
 * thin fixed width so the active runtime fills the chart instead of being squeezed
 * by hours of off time.
 */
private fun layoutSamples(s: List<BatterySample>): ChartLayout {
    if (s.isEmpty()) return ChartLayout(emptyList(), emptyList(), emptyList())
    val offThresh = 8.0 / 60.0    // gap > 8 min => unit was off
    val gapWidth = 4.0 / 60.0     // draw each off period as a 4-min-wide break
    val points = ArrayList<PlotPoint>(s.size)
    val marks = arrayListOf(0.0 to s[0].timeLabel.take(5))
    val offBands = ArrayList<Pair<Double, Double>>()
    var x = 0.0
    var session = 0
    for ((i, sample) in s.withIndex()) {
        if (i > 0) {
            val gap = sample.hours - s[i - 1].hours
            if (gap > offThresh) {
                offBands.add(x to x + gapWidth)
                x += gapWidth
                session += 1
                marks.add(x to sample.timeLabel.take(5))
            } else {
                x += gap
            }
        }
        points.add(
            PlotPoint(x, sample.percent, sample.currentMa, session, sample.timeLabel.take(5)),
        )
    }
    return ChartLayout(points, marks, offBands)
}

/**
 * Drain measured over DISCHARGING time only.
 *
 * The naive version — (first% − last%) / total elapsed — is destroyed by any
 * mid-log charge: charge back up and the net drop collapses while the clock keeps
 * running, so the rate reads far too low and "Est. left" far too high. On a real
 * log that went 99% → 47%, charged to 100%, then ran down to 80% over 9.5 h, it
 * reported 2.0 %/hr and 40 h left; the true figures are 10.8 %/hr and 7.4 h.
 *
 * So accumulate drop and time only across adjacent pairs that are genuinely
 * discharging: skip any pair where the percentage ROSE (charging), and any pair
 * separated by more than the off-threshold (the unit was asleep, and that
 * wall-clock time is not runtime).
 */
private fun drainStats(s: List<BatterySample>): Pair<Double, Double>? {
    val offThresh = 8.0 / 60.0
    var drop = 0.0
    var hours = 0.0
    for (i in 1 until s.size) {
        val a = s[i - 1]
        val b = s[i]
        val gap = b.hours - a.hours
        if (gap > offThresh) continue        // unit was off
        if (b.percent > a.percent) continue  // charging
        drop += a.percent - b.percent
        hours += gap
    }
    // Average only samples actually drawing, and as a positive number to match
    // the flipped chart.
    val draws = s.map { it.currentMa }.filter { it < 0 }.map { -it }
    val avg = if (draws.isEmpty()) 0.0 else draws.sum() / draws.size
    if (hours <= 0.01 || drop <= 0) return null
    return (drop / hours) to avg
}

@Composable
private fun BatteryChart(
    layout: ChartLayout,
    value: (PlotPoint) -> Double,
    color: Color,
    yDomain: Pair<Double, Double>?,
    selected: Int?,
    onSelect: (Int?) -> Unit,
    format: (Double) -> String,
    modifier: Modifier = Modifier,
) {
    val points = layout.points
    if (points.isEmpty()) return
    val xMin = points.first().x
    val xMax = points.last().x.coerceAtLeast(xMin + 1e-6)
    val values = points.map(value)
    val (yMin, yMax) = yDomain ?: run {
        val lo = values.min()
        val hi = values.max()
        val pad = ((hi - lo) * 0.1).coerceAtLeast(1.0)
        (lo - pad) to (hi + pad)
    }

    Box(modifier) {
        Canvas(
            Modifier
                .fillMaxSize()
                .pointerInput(points.size) {
                    detectTapGestures { offset ->
                        // The x axis is the COMPRESSED position, not elapsed
                        // time, so snap to the nearest plotted point rather than
                        // interpolating — otherwise a tap inside a collapsed
                        // off-band would report a value that was never sampled.
                        val fraction = (offset.x / size.width).coerceIn(0f, 1f)
                        val target = xMin + (xMax - xMin) * fraction
                        onSelect(points.indices.minByOrNull { abs(points[it].x - target) })
                    }
                },
        ) {
            fun px(x: Double) = (((x - xMin) / (xMax - xMin)) * size.width).toFloat()
            fun py(v: Double) =
                (size.height - ((v - yMin) / (yMax - yMin)) * size.height).toFloat()

            for ((start, end) in layout.offBands) {
                drawRect(
                    Palette.muted.copy(alpha = 0.12f),
                    topLeft = Offset(px(start), 0f),
                    size = androidx.compose.ui.geometry.Size(px(end) - px(start), size.height),
                )
            }

            // One path per awake run, so the line breaks across an off period.
            var i = 0
            while (i < points.size) {
                val session = points[i].session
                val path = Path()
                var first = true
                while (i < points.size && points[i].session == session) {
                    val p = points[i]
                    if (first) {
                        path.moveTo(px(p.x), py(value(p))); first = false
                    } else {
                        path.lineTo(px(p.x), py(value(p)))
                    }
                    i += 1
                }
                drawPath(path, color, style = Stroke(width = 2.5f))
            }

            selected?.let { index ->
                val p = points.getOrNull(index) ?: return@let
                drawLine(
                    Palette.muted.copy(alpha = 0.35f),
                    Offset(px(p.x), 0f),
                    Offset(px(p.x), size.height),
                    strokeWidth = 1f,
                    pathEffect = PathEffect.dashPathEffect(floatArrayOf(6f, 6f)),
                )
                drawCircle(color, radius = 5f, center = Offset(px(p.x), py(value(p))))
            }
            drawAxis(layout, ::px)
        }

        selected?.let { index ->
            val p = points.getOrNull(index) ?: return@let
            Column(
                Modifier
                    .align(Alignment.TopStart)
                    .padding(8.dp)
                    .background(Palette.surface)
                    .padding(horizontal = 8.dp, vertical = 5.dp),
            ) {
                Text(
                    format(value(p)),
                    style = barlow(13.sp, FontWeight.Bold),
                    color = Palette.ink,
                )
                Text(p.timeLabel, style = barlow(10.sp), color = Palette.muted)
            }
        }
    }
}

private fun DrawScope.drawAxis(layout: ChartLayout, px: (Double) -> Float) {
    for ((x, _) in layout.marks) {
        drawLine(
            Palette.hairline,
            Offset(px(x), 0f),
            Offset(px(x), size.height),
            strokeWidth = 1f,
        )
    }
}

@Composable
private fun LogTab(text: String) {
    Box(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .horizontalScroll(rememberScrollState()),
    ) {
        Text(
            text.ifEmpty { "(empty log)" },
            style = barlow(11.sp).copy(fontFamily = FontFamily.Monospace),
            color = Palette.ink,
            modifier = Modifier.padding(16.dp),
        )
    }
}

/** One parsed "battery:" log entry. */
class BatterySample(
    val hours: Double,       // elapsed hours from the first sample
    val percent: Double,
    val currentMa: Double,
    val timeLabel: String,
) {
    companion object {
        /**
         * Parses lines like:
         * `[14:32:10] battery: 87% 3912mV -142mA 1740/2000mAh discharging`
         */
        fun parse(text: String): List<BatterySample> {
            val out = ArrayList<BatterySample>()
            var base: Double? = null
            var dayOffset = 0.0
            var lastSod: Double? = null
            for (line in text.lineSequence()) {
                val batIndex = line.indexOf("battery:")
                if (batIndex < 0) continue
                val lb = line.indexOf('[')
                val rb = line.indexOf(']')
                if (lb < 0 || rb < 0 || lb >= rb) continue
                val ts = line.substring(lb + 1, rb)      // "HH:MM:SS"
                val parts = ts.split(":")
                if (parts.size != 3) continue
                val h = parts[0].toDoubleOrNull() ?: continue
                val m = parts[1].toDoubleOrNull() ?: continue
                val sec = parts[2].toDoubleOrNull() ?: continue
                var sod = h * 3600 + m * 60 + sec
                val last = lastSod
                if (last != null && sod < last - 1) dayOffset += 86400   // past midnight
                lastSod = sod
                sod += dayOffset
                if (base == null) base = sod

                val rest = line.substring(batIndex + "battery:".length)
                val pct = numberBefore("%", rest) ?: continue
                val ma = numberBefore("mA ", rest) ?: 0.0
                out.add(BatterySample((sod - (base ?: sod)) / 3600, pct, ma, ts))
            }
            return out
        }

        /** The signed/decimal number immediately preceding [unit] in [s]. */
        private fun numberBefore(unit: String, s: String): Double? {
            val at = s.indexOf(unit)
            if (at < 0) return null
            var i = at
            val chars = StringBuilder()
            while (i > 0) {
                val c = s[i - 1]
                if (c.isDigit() || c == '.' || c == '-') {
                    chars.insert(0, c); i -= 1
                } else {
                    break
                }
            }
            return chars.toString().toDoubleOrNull()
        }
    }
}
