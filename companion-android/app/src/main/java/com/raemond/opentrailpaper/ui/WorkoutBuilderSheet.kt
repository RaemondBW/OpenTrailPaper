package com.raemond.opentrailpaper.ui

import androidx.compose.animation.animateContentSize
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.ExpandLess
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material.icons.filled.Remove
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.OutlinedTextFieldDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.toMutableStateList
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.data.LocalWorkouts
import java.util.Locale
import kotlin.math.max
import kotlin.math.roundToInt

// The workout editor, taking its structure from the Claude Design "Workout
// Editor" frame — collapsed rows showing only what defines a block, ONE row
// open at a time with the zone strip and destructive action beneath it, a
// profile chart that is a control (tap a bar to open its block), and orange
// reserved for commit and the open block. Port of WorkoutBuilderView in
// companion-ios/Sources/WorkoutsView.swift, in the app's Material dress.

val WORKOUT_ZONES = listOf(
    "Recovery" to 50, "Endurance" to 65, "Tempo" to 80, "Threshold" to 100,
    "VO2 max" to 115, "Anaerobic" to 135, "Sprint" to 170,
)

fun workoutZoneForPct(pct: Int): Int = when {
    pct <= 55 -> 1
    pct <= 75 -> 2
    pct <= 90 -> 3
    pct <= 105 -> 4
    pct <= 120 -> 5
    pct <= 150 -> 6
    else -> 7
}

data class WorkoutBlock(
    val minutes: Double,
    val zone: Int,          // 1...7
    val key: Long = nextKey++,
) {
    val pct: Int get() = WORKOUT_ZONES[zone - 1].second

    companion object {
        private var nextKey = 1L
    }
}

/**
 * A workout pulled off the device or the phone library, parsed into the
 * builder's blocks. ERG (watts) files convert to %FTP with the device's FTP;
 * ramps flatten to their average — the builder speaks flat blocks.
 */
data class WorkoutSeed(val file: String, val name: String, val blocks: List<WorkoutBlock>) {
    companion object {
        fun parse(file: String, text: String, ftp: Int): WorkoutSeed? {
            val ftpW = if (ftp > 0) ftp else 250
            var percent = false
            var inData = false
            val pts = mutableListOf<Pair<Double, Double>>()
            for (raw in text.split("\n")) {
                val line = raw.trim()
                val upper = line.uppercase()
                if (!inData) {
                    if (upper.contains("[COURSE DATA]")) inData = true
                    else if (upper.contains("PERCENT")) percent = true
                } else if (upper.contains("[END COURSE DATA]")) {
                    break
                } else {
                    val tok = line.split(' ', '\t').filter { it.isNotEmpty() }
                    if (tok.size >= 2) {
                        val m = tok[0].toDoubleOrNull()
                        val v = tok[1].toDoubleOrNull()
                        if (m != null && v != null) pts.add(m to v)
                    }
                }
            }
            if (pts.size < 2) return null
            val out = mutableListOf<WorkoutBlock>()
            for (i in 1 until pts.size) {
                val dur = pts[i].first - pts[i - 1].first
                if (dur <= 0.001) continue
                val avg = (pts[i - 1].second + pts[i].second) / 2
                val pct = if (percent) avg else avg * 100 / ftpW
                out.add(
                    WorkoutBlock(
                        minutes = (dur * 2).roundToInt() / 2.0,
                        zone = workoutZoneForPct(pct.roundToInt()),
                    ),
                )
            }
            if (out.isEmpty()) return null
            val stem = file.substringBeforeLast('.', file)
            return WorkoutSeed(file, stem.replace('_', ' '), out)
        }
    }
}

@Composable
fun WorkoutBuilderSheet(ble: BleManager, seed: WorkoutSeed?, onDismiss: () -> Unit) {
    val context = LocalContext.current
    var name by remember { mutableStateOf(seed?.name ?: "My Workout") }
    val blocks = remember {
        (seed?.blocks ?: listOf(
            WorkoutBlock(10.0, 2),
            WorkoutBlock(5.0, 4),
            WorkoutBlock(3.0, 1),
            WorkoutBlock(5.0, 4),
            WorkoutBlock(5.0, 1),
        )).toMutableStateList()
    }
    var openKey by remember { mutableLongStateOf(blocks.firstOrNull()?.key ?: -1L) }

    val ftpW = if (ble.ftpWatts > 0) ble.ftpWatts else 250
    val totalMin = blocks.sumOf { it.minutes }

    fun send() {
        val safe = name.trim().replace(' ', '_')
            .filter { it.isLetterOrDigit() || it == '_' || it == '-' }
        // Editing keeps the original filename so Send is an overwrite, not a
        // sibling — even if the display name was reworded.
        val file = seed?.file
            ?: ((safe.ifEmpty { "workout" }).lowercase() + ".mrc")
        val s = buildString {
            append("[COURSE HEADER]\nVERSION = 2\nUNITS = ENGLISH\n")
            append("DESCRIPTION = $name\nFILE NAME = $file\n")
            append("MINUTES PERCENT\n[END COURSE HEADER]\n[COURSE DATA]\n")
            var t = 0.0
            for (b in blocks) {
                append(String.format(Locale.US, "%.2f %d\n", t, b.pct))
                t += b.minutes
                append(String.format(Locale.US, "%.2f %d\n", t, b.pct))
            }
            append("[END COURSE DATA]\n")
        }
        // The phone keeps the master copy either way; the device gets it now
        // if it's connected, or from the library row later.
        LocalWorkouts.save(context, file, s)
        if (ble.state == BleManager.ConnState.CONNECTED) {
            ble.uploadWorkout(file, s)
            ble.workoutMessage = "Saved on phone — sending $file…"
        } else {
            ble.workoutMessage = "Saved on phone — device not connected"
        }
        onDismiss()
    }

    FullScreenSheet(
        title = if (seed == null) "New Workout" else "Edit Workout",
        onDismiss = onDismiss,
        confirmLabel = "Cancel",
        trailing = {
            TextButton(
                enabled = blocks.isNotEmpty() && name.isNotBlank(),
                onClick = { send() },
            ) {
                Text("Send", style = TypeScale.bodyStrong)
            }
        },
    ) {
        Column(
            Modifier
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Card {
                TrackedLabel("Workout name")
                OutlinedTextField(
                    value = name,
                    onValueChange = { name = it },
                    textStyle = condensed(30.sp, FontWeight.Bold),
                    colors = OutlinedTextFieldDefaults.colors(
                        focusedBorderColor = Color.Transparent,
                        unfocusedBorderColor = Color.Transparent,
                        focusedTextColor = Palette.ink,
                        unfocusedTextColor = Palette.ink,
                    ),
                    modifier = Modifier.fillMaxWidth(),
                )
            }

            // The profile is a control: tap a bar to open that block's row.
            // Height is intensity, the open block is orange, hard blocks
            // (>=95% FTP) are ink — the device page's own encoding.
            Card {
                val maxPct = max(blocks.maxOfOrNull { it.pct } ?: 100, 100)
                Row(
                    Modifier.fillMaxWidth().height(74.dp),
                    horizontalArrangement = Arrangement.spacedBy(2.dp),
                    verticalAlignment = Alignment.Bottom,
                ) {
                    for (b in blocks) {
                        val h = 14 + 56 * b.pct / maxPct
                        Box(
                            Modifier
                                .weight(b.minutes.toFloat().coerceAtLeast(0.5f))
                                .height(h.dp)
                                .clip(RoundedCornerShape(topStart = 3.dp, topEnd = 3.dp))
                                .background(
                                    when {
                                        b.key == openKey -> Palette.accent
                                        b.pct >= 95 -> Palette.ink
                                        else -> Palette.hairline
                                    },
                                )
                                .clickable { openKey = b.key },
                        )
                    }
                }
                Row(Modifier.padding(top = 8.dp)) {
                    Text(
                        "${blocks.size} BLOCKS · ${totalMin.roundToInt()} MIN",
                        style = barlow(11.sp, FontWeight.SemiBold),
                        color = Palette.muted,
                        modifier = Modifier.weight(1f),
                    )
                    Text(
                        "FTP $ftpW W",
                        style = barlow(11.sp, FontWeight.SemiBold),
                        color = Palette.muted,
                    )
                }
            }

            Card {
                blocks.forEachIndexed { i, b ->
                    if (i > 0) HorizontalDivider(color = Palette.hairline)
                    BlockRow(
                        block = b,
                        ftpW = ftpW,
                        open = b.key == openKey,
                        onToggle = { openKey = if (b.key == openKey) -1L else b.key },
                        onChange = { blocks[i] = it },
                        onCopy = {
                            val dup = WorkoutBlock(b.minutes, b.zone)
                            blocks.add(i + 1, dup)
                            openKey = dup.key
                        },
                        onDelete = {
                            blocks.removeAt(i)
                            openKey = -1L
                        },
                    )
                }
            }

            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                BottomButton("Add block", Modifier.weight(1f)) {
                    val nb = WorkoutBlock(5.0, 4)
                    blocks.add(nb)
                    openKey = nb.key
                }
                BottomButton("Recovery", Modifier.weight(1f)) {
                    val nb = WorkoutBlock(3.0, 1)
                    blocks.add(nb)
                    openKey = nb.key
                }
            }
        }
    }
}

@Composable
private fun BlockRow(
    block: WorkoutBlock,
    ftpW: Int,
    open: Boolean,
    onToggle: () -> Unit,
    onChange: (WorkoutBlock) -> Unit,
    onCopy: () -> Unit,
    onDelete: () -> Unit,
) {
    val zoneName = WORKOUT_ZONES[block.zone - 1].first
    val watts = (ftpW * block.pct / 100 / 5) * 5
    Column(
        Modifier
            .fillMaxWidth()
            .animateContentSize(),
    ) {
        // Collapsed face: chip / name+power / duration / caret.
        Row(
            Modifier
                .clickable(onClick = onToggle)
                .padding(vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text(
                "Z${block.zone}",
                style = barlow(15.sp, FontWeight.SemiBold),
                color = if (block.pct >= 95) Color.White else Palette.ink,
                textAlign = TextAlign.Center,
                modifier = Modifier
                    .size(42.dp)
                    .clip(RoundedCornerShape(9.dp))
                    .background(if (block.pct >= 95) Palette.ink else Palette.paper)
                    .padding(top = 10.dp),
            )
            Column(Modifier.weight(1f)) {
                Text(zoneName, style = TypeScale.bodyStrong, color = Palette.ink)
                Text(
                    "$watts W · ${block.pct}% FTP",
                    style = barlow(13.sp, FontWeight.Medium),
                    color = Palette.muted,
                )
            }
            Text(fmtDur(block.minutes), style = condensed(26.sp, FontWeight.Bold), color = Palette.ink)
            Icon(
                if (open) Icons.Filled.ExpandLess else Icons.Filled.ExpandMore,
                contentDescription = null,
                tint = Palette.faint,
            )
        }

        if (open) {
            Column(
                Modifier.padding(bottom = 14.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    for (z in 1..7) {
                        Text(
                            "Z$z",
                            style = barlow(14.sp, FontWeight.SemiBold),
                            color = if (z == block.zone) Color.White else Palette.muted,
                            textAlign = TextAlign.Center,
                            modifier = Modifier
                                .weight(1f)
                                .height(42.dp)
                                .clip(RoundedCornerShape(9.dp))
                                .background(if (z == block.zone) Palette.ink else Palette.paper)
                                .clickable { onChange(block.copy(zone = z)) }
                                .padding(top = 10.dp),
                        )
                    }
                }
                Row(
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Row(
                        Modifier
                            .weight(1f)
                            .clip(RoundedCornerShape(10.dp))
                            .background(Palette.paper),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        IconButton(onClick = {
                            onChange(block.copy(minutes = (block.minutes - 1).coerceAtLeast(1.0)))
                        }) {
                            Icon(Icons.Filled.Remove, contentDescription = "Shorter", tint = Palette.ink)
                        }
                        Text(
                            fmtDur(block.minutes),
                            style = condensed(22.sp, FontWeight.Bold),
                            color = Palette.ink,
                            textAlign = TextAlign.Center,
                            modifier = Modifier.weight(1f),
                        )
                        IconButton(onClick = {
                            onChange(block.copy(minutes = (block.minutes + 1).coerceAtMost(60.0)))
                        }) {
                            Icon(Icons.Filled.Add, contentDescription = "Longer", tint = Palette.ink)
                        }
                    }
                    TextButton(onClick = onCopy) {
                        Text("Copy", style = barlow(14.sp, FontWeight.SemiBold), color = Palette.ink)
                    }
                    TextButton(onClick = onDelete) {
                        Text("Delete", style = barlow(14.sp, FontWeight.SemiBold), color = Palette.accent)
                    }
                }
            }
        }
    }
}

@Composable
private fun BottomButton(label: String, modifier: Modifier = Modifier, action: () -> Unit) {
    Row(
        modifier
            .height(48.dp)
            .clip(RoundedCornerShape(12.dp))
            .background(Palette.surface)
            .clickable(onClick = action),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.Center,
    ) {
        Icon(Icons.Filled.Add, contentDescription = null, tint = Palette.ink,
             modifier = Modifier.size(16.dp))
        Spacer(Modifier.width(6.dp))
        Text(label, style = barlow(15.sp, FontWeight.SemiBold), color = Palette.ink)
    }
}

private fun fmtDur(m: Double): String {
    val sec = (m * 60).roundToInt()
    return "${sec / 60}:" + "%02d".format(sec % 60)
}
