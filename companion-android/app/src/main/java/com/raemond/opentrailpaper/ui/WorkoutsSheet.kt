package com.raemond.opentrailpaper.ui

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
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
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.ArrowUpward
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.DirectionsBike
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.SkipNext
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.data.LocalWorkouts

// Structured workouts: what's loaded on the head unit, the session's live
// state, and the ways to get a workout onto the device — import an .erg/.mrc
// export, or build/edit one here block by block. Everything speaks
// CHR_WORKOUT; the device stores files in /workouts on its card. Port of
// WorkoutsView in companion-ios/Sources/WorkoutsView.swift.

@Composable
fun WorkoutsSheet(ble: BleManager, onDismiss: () -> Unit) {
    val context = LocalContext.current
    var showBuilder by remember { mutableStateOf(false) }
    var editSeed by remember { mutableStateOf<WorkoutSeed?>(null) }
    var localWorkouts by remember { mutableStateOf(LocalWorkouts.list(context)) }

    LaunchedEffect(Unit) { ble.refreshWorkouts() }

    // A workout pulled off the device: the editor opens when the file arrives.
    LaunchedEffect(ble.fetchedWorkout) {
        val f = ble.fetchedWorkout ?: return@LaunchedEffect
        ble.fetchedWorkout = null
        val seed = WorkoutSeed.parse(f.name, f.text, ble.ftpWatts)
        if (seed != null) editSeed = seed
        else ble.workoutMessage = "Couldn't parse ${f.name}"
    }

    val importer = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument(),
    ) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        val text = runCatching {
            context.contentResolver.openInputStream(uri)?.use {
                it.readBytes().toString(Charsets.UTF_8)
            }
        }.getOrNull()
        if (text == null) {
            ble.workoutMessage = "Couldn't read that file"
            return@rememberLauncherForActivityResult
        }
        val name = uri.lastPathSegment?.substringAfterLast('/') ?: "imported.mrc"
        val ext = name.substringAfterLast('.', "").lowercase()
        if (ext != "erg" && ext != "mrc" && !text.contains("[COURSE DATA]")) {
            ble.workoutMessage = "Not an .erg/.mrc workout"
            return@rememberLauncherForActivityResult
        }
        LocalWorkouts.save(context, name, text)
        localWorkouts = LocalWorkouts.list(context)
        ble.uploadWorkout(name, text)
        ble.workoutMessage = "Sending $name…"
    }

    FullScreenSheet(title = "Workouts", onDismiss = onDismiss) {
        Box(Modifier.fillMaxSize()) {
            Column(
                Modifier
                    .verticalScroll(rememberScrollState())
                    .padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                if (ble.workoutStatus.loaded) SessionSection(ble)
                LibrarySection(
                    ble = ble,
                    local = localWorkouts,
                    onEdit = { item ->
                        if (item.onPhone) {
                            val text = LocalWorkouts.read(context, item.name)
                            val seed = text?.let {
                                WorkoutSeed.parse(item.name, it, ble.ftpWatts)
                            }
                            if (seed != null) editSeed = seed
                            else ble.workoutMessage = "Couldn't parse ${item.name}"
                        } else {
                            ble.fetchWorkout(item.name)   // editor opens on arrival
                        }
                    },
                    onSendOrLoad = { item ->
                        val text = if (item.onPhone) {
                            LocalWorkouts.read(context, item.name)
                        } else {
                            null
                        }
                        if (text != null) {
                            ble.uploadWorkout(item.name, text)   // saves + loads
                            ble.workoutMessage = "Sending ${item.name}…"
                        } else if (item.onDevice) {
                            ble.loadWorkout(item.name)
                        }
                    },
                    onDelete = { item ->
                        if (item.onPhone) {
                            LocalWorkouts.delete(context, item.name)
                            localWorkouts = LocalWorkouts.list(context)
                        }
                        if (item.onDevice) ble.deleteWorkout(item.name)
                    },
                )

                TrackedLabel("Add a workout")
                Card {
                    Row(
                        Modifier.clickable { showBuilder = true },
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Icon(Icons.Filled.Add, contentDescription = null, tint = Palette.accent)
                        Spacer(Modifier.size(10.dp))
                        Text("Create a workout", style = TypeScale.body, color = Palette.ink)
                    }
                    HorizontalDivider(Modifier.padding(vertical = 8.dp), color = Palette.hairline)
                    Row(
                        Modifier.clickable { importer.launch(arrayOf("*/*")) },
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Icon(Icons.Filled.Download, contentDescription = null, tint = Palette.accent)
                        Spacer(Modifier.size(10.dp))
                        Text("Import .erg / .mrc file", style = TypeScale.body, color = Palette.ink)
                    }
                }
            }

            // Transient status toast, matching the iOS bottom capsule.
            val msg = ble.workoutMessage
            if (msg != null) {
                LaunchedEffect(msg) {
                    kotlinx.coroutines.delay(2500)
                    if (ble.workoutMessage == msg) ble.workoutMessage = null
                }
                Text(
                    msg,
                    style = TypeScale.body,
                    color = Palette.ink,
                    modifier = Modifier
                        .align(Alignment.BottomCenter)
                        .padding(bottom = 12.dp)
                        .background(Palette.surface, RoundedCornerShape(50))
                        .padding(horizontal = 14.dp, vertical = 8.dp),
                )
            }
        }
    }

    if (showBuilder) {
        WorkoutBuilderSheet(ble, seed = null) {
            showBuilder = false
            localWorkouts = LocalWorkouts.list(context)
        }
    }
    editSeed?.let { seed ->
        WorkoutBuilderSheet(ble, seed = seed) {
            editSeed = null
            localWorkouts = LocalWorkouts.list(context)
        }
    }
}

// MARK: live session

/** With nothing live the page IS the picker, so this section only exists
 * while a workout is loaded. */
@Composable
private fun SessionSection(ble: BleManager) {
    val s = ble.workoutStatus
    TrackedLabel("Session")
    Card {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                s.name,
                style = TypeScale.bodyStrong,
                color = Palette.ink,
                modifier = Modifier.weight(1f),
            )
            Text(
                when {
                    s.done -> "DONE"
                    s.running -> "RUNNING"
                    s.paused -> "PAUSED"
                    else -> "READY"
                },
                style = barlow(12.sp, FontWeight.Bold),
                color = if (s.running) Color(0xFF2E7D32) else Palette.muted,
            )
        }
        Row(Modifier.padding(top = 4.dp)) {
            Text(
                "Block ${s.blockIndex + 1} of ${s.blockCount}",
                style = TypeScale.body,
                color = Palette.muted,
                modifier = Modifier.weight(1f),
            )
            Text("Target ${s.targetW} W", style = TypeScale.body, color = Palette.muted)
        }
        LinearProgressIndicator(
            progress = {
                (s.elapsedSec.toFloat() / s.totalSec.coerceAtLeast(1).toFloat())
                    .coerceIn(0f, 1f)
            },
            color = Palette.accent,
            trackColor = Palette.hairline,
            modifier = Modifier.fillMaxWidth().padding(vertical = 8.dp),
        )
        Row {
            Text(
                clock(s.elapsedSec),
                style = barlow(12.sp, FontWeight.Medium),
                color = Palette.muted,
                modifier = Modifier.weight(1f),
            )
            Text(
                "-" + clock((s.totalSec - s.elapsedSec).coerceAtLeast(0)),
                style = barlow(12.sp, FontWeight.Medium),
                color = Palette.muted,
            )
        }

        HorizontalDivider(Modifier.padding(vertical = 8.dp), color = Palette.hairline)
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("Pause after every block", style = TypeScale.body, color = Palette.ink)
                Text(
                    "Holds at each boundary until you resume",
                    style = barlow(13.sp),
                    color = Palette.muted,
                )
            }
            Switch(
                checked = s.pauseEachBlock,
                onCheckedChange = { ble.setWorkoutPauseEachBlock(it) },
                enabled = ble.state == BleManager.ConnState.CONNECTED,
                colors = SwitchDefaults.colors(
                    checkedThumbColor = Color.White,
                    checkedTrackColor = Palette.accent,
                ),
            )
        }

        Row(
            Modifier.padding(top = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            // The one "go" action is filled accent, the rest are outlined ink.
            when {
                s.running -> ControlButton(
                    Icons.Filled.Pause, "Pause", prominent = true,
                    modifier = Modifier.weight(1f),
                ) { ble.workoutPause() }
                s.paused -> ControlButton(
                    Icons.Filled.PlayArrow, "Resume", prominent = true,
                    modifier = Modifier.weight(1f),
                ) { ble.workoutResume() }
                else -> ControlButton(
                    Icons.Filled.PlayArrow, "Start", prominent = true,
                    modifier = Modifier.weight(1f),
                ) { ble.workoutStart() }
            }
            ControlButton(
                Icons.Filled.SkipNext, "Skip",
                enabled = s.running || s.paused,
                modifier = Modifier.weight(1f),
            ) { ble.workoutSkip() }
            ControlButton(
                Icons.Filled.Stop, "Stop",
                modifier = Modifier.weight(1f),
            ) { ble.workoutUnload() }
        }
    }
}

@Composable
private fun ControlButton(
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    label: String,
    modifier: Modifier = Modifier,
    prominent: Boolean = false,
    enabled: Boolean = true,
    action: () -> Unit,
) {
    if (prominent) {
        androidx.compose.material3.Button(
            onClick = action,
            enabled = enabled,
            colors = androidx.compose.material3.ButtonDefaults.buttonColors(
                containerColor = Palette.accent,
            ),
            shape = RoundedCornerShape(10.dp),
            modifier = modifier.height(40.dp),
        ) {
            Icon(icon, contentDescription = null, modifier = Modifier.size(16.dp))
            Spacer(Modifier.size(6.dp))
            Text(label, style = barlow(15.sp, FontWeight.SemiBold))
        }
    } else {
        OutlinedButton(
            onClick = action,
            enabled = enabled,
            shape = RoundedCornerShape(10.dp),
            modifier = modifier.height(40.dp),
        ) {
            Icon(icon, contentDescription = null, tint = Palette.ink,
                 modifier = Modifier.size(16.dp))
            Spacer(Modifier.size(6.dp))
            Text(label, style = barlow(15.sp, FontWeight.SemiBold), color = Palette.ink)
        }
    }
}

// MARK: the library — phone and device as one list

/** One row per workout name, wherever it lives. The phone copy is the
 * editable master; the device copy is what the head unit can ride. */
data class WorkoutItem(val name: String, val onPhone: Boolean, val onDevice: Boolean)

@Composable
private fun LibrarySection(
    ble: BleManager,
    local: List<String>,
    onEdit: (WorkoutItem) -> Unit,
    onSendOrLoad: (WorkoutItem) -> Unit,
    onDelete: (WorkoutItem) -> Unit,
) {
    val phone = local.toSet()
    val device = ble.deviceWorkouts.toSet()
    val items = (phone + device).sorted().map {
        WorkoutItem(it, phone.contains(it), device.contains(it))
    }
    // With no live workout the page is a PICKER: tapping a row loads it on
    // the device. With a session live, tapping edits — the arrow still
    // loads/sends either way.
    val pickerMode = !ble.workoutStatus.loaded

    TrackedLabel(if (pickerMode) "Pick a workout" else "Workouts")
    Card {
        if (items.isEmpty()) {
            Text(
                "Workouts you create or import appear here.",
                style = TypeScale.body,
                color = Palette.muted,
            )
        }
        items.forEachIndexed { i, item ->
            if (i > 0) HorizontalDivider(color = Palette.hairline)
            Row(
                Modifier
                    .clickable { if (pickerMode) onSendOrLoad(item) else onEdit(item) }
                    .padding(vertical = 10.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Icon(
                    Icons.Filled.DirectionsBike,
                    contentDescription = null,
                    tint = Palette.muted,
                )
                Spacer(Modifier.size(10.dp))
                Column(Modifier.weight(1f)) {
                    Text(item.name, style = TypeScale.bodyStrong, color = Palette.ink)
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        if (item.onPhone) {
                            Text("Phone", style = barlow(12.sp), color = Palette.muted)
                        }
                        if (item.onDevice) {
                            Text("Device", style = barlow(12.sp), color = Palette.muted)
                        }
                        if (isLoaded(ble, item.name)) {
                            val s = ble.workoutStatus
                            Text(
                                when {
                                    s.done -> "DONE"
                                    s.running -> "RUNNING"
                                    s.paused -> "PAUSED"
                                    else -> "LOADED"
                                },
                                style = barlow(12.sp, FontWeight.Bold),
                                color = if (s.running) Color(0xFF2E7D32) else Palette.accent,
                            )
                        }
                    }
                }
                IconButton(onClick = { onEdit(item) }) {
                    Icon(Icons.Filled.Edit, contentDescription = "Edit", tint = Palette.muted)
                }
                IconButton(
                    onClick = { onSendOrLoad(item) },
                    enabled = ble.state == BleManager.ConnState.CONNECTED,
                ) {
                    if (isLoaded(ble, item.name)) {
                        Icon(
                            Icons.Filled.CheckCircle,
                            contentDescription = "Loaded",
                            tint = Color(0xFF2E7D32),
                        )
                    } else {
                        Icon(
                            Icons.Filled.ArrowUpward,
                            contentDescription = "Send to device",
                            tint = Palette.accent,
                        )
                    }
                }
                IconButton(onClick = { onDelete(item) }) {
                    Icon(Icons.Filled.Delete, contentDescription = "Delete", tint = Palette.muted)
                }
            }
        }
    }
    Text(
        if (pickerMode) {
            "Tap a workout to load it on the device."
        } else {
            "Tap to edit. The arrow sends it to the device and loads it; " +
                "phone copies are the editable masters."
        },
        style = barlow(13.sp),
        color = Palette.muted,
    )
}

/** The device page shows the name uppercased without its extension; match on
 * that so the checkmark lands on the right row. */
private fun isLoaded(ble: BleManager, file: String): Boolean {
    val stem = file.substringBeforeLast('.', file)
    return ble.workoutStatus.loaded &&
        ble.workoutStatus.name.equals(stem, ignoreCase = true)
}

private fun clock(sec: Long): String = "%d:%02d".format(sec / 60, sec % 60)
