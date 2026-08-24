package com.raemond.opentrailpaper.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
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
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ChevronRight
import androidx.compose.material.icons.filled.DirectionsBike
import androidx.compose.material.icons.filled.GridView
import androidx.compose.material.icons.filled.LocationOn
import androidx.compose.material.icons.filled.StarBorder
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.ble.BikeSensor
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.ble.DeviceStatus
import com.raemond.opentrailpaper.data.Units
import java.util.Locale

/**
 * Live device status — connection, GPS, battery, and ride data streamed over
 * BLE. Mirrors the head unit's dashboard at a glance.
 */
@Composable
fun RideScreen(ble: BleManager) {
    var showDashEditor by remember { mutableStateOf(false) }
    var showWorkouts by remember { mutableStateOf(false) }
    val s = ble.status
    val useMiles = ble.useMiles

    LaunchedEffect(ble.state) {
        if (ble.state == BleManager.ConnState.CONNECTED) ble.refreshSensors()
    }

    Column(
        Modifier
            .fillMaxSize()
            .background(Palette.paper)
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        Header(ble, s)
        GpsCard(s)
        SpeedCard(s, useMiles)
        Row(horizontalArrangement = Arrangement.spacedBy(14.dp)) {
            SensorCard(
                "Heart Rate", s.heartRate?.toString(), "bpm",
                ble.connectedSensor(1), ble.state, Modifier.weight(1f),
            )
            SensorCard(
                "Power", s.power?.toString(), "W",
                ble.connectedSensor(2), ble.state, Modifier.weight(1f),
            )
        }
        if (s.hasRoute) RouteCard(s, useMiles)
        WorkoutsCard(ble) { showWorkouts = true }
        DashboardCard(ble) { showDashEditor = true }
        Spacer(Modifier.height(8.dp))
        if (ble.state != BleManager.ConnState.CONNECTED) {
            PrimaryButton(
                title = if (ble.state == BleManager.ConnState.SCANNING ||
                    ble.state == BleManager.ConnState.CONNECTING
                ) {
                    "Searching…"
                } else {
                    "Connect to OpenTrailPaper"
                },
                icon = Icons.Filled.Wifi,
            ) { ble.startScan() }
        }
    }

    if (showDashEditor) {
        DashboardEditorSheet(ble) { showDashEditor = false }
    }
    if (showWorkouts) {
        WorkoutsSheet(ble) { showWorkouts = false }
    }
}

/** Structured workouts: load/build one, and the live session at a glance. */
@Composable
private fun WorkoutsCard(ble: BleManager, onOpen: () -> Unit) {
    Card(Modifier.clickable(onClick = onOpen)) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            Icon(
                Icons.Filled.DirectionsBike,
                contentDescription = null,
                tint = Palette.muted,
                modifier = Modifier.width(62.dp),
            )
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("Workouts", style = TypeScale.title, color = Palette.ink)
                val w = ble.workoutStatus
                Text(
                    if (w.loaded) {
                        val state = when {
                            w.running -> "running"
                            w.paused -> "paused"
                            w.done -> "done"
                            else -> "ready"
                        }
                        "${w.name} — block ${w.blockIndex + 1}/${w.blockCount}, $state"
                    } else {
                        "Send a structured workout to the device, or build one"
                    },
                    style = TypeScale.body,
                    color = Palette.muted,
                )
            }
            Icon(Icons.Filled.ChevronRight, contentDescription = null, tint = Palette.faint)
        }
    }
}

/** "Ride" title + a compact device-status pill (green dot + name + battery). */
@Composable
private fun Header(ble: BleManager, s: DeviceStatus) {
    Row(
        Modifier.fillMaxWidth().padding(top = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text("Ride", style = TypeScale.screenTitle, color = Palette.ink)
        Spacer(Modifier.weight(1f))
        Row(
            Modifier
                .background(Palette.surface, RoundedCornerShape(50))
                .border(1.dp, Palette.hairline, RoundedCornerShape(50))
                .padding(horizontal = 12.dp, vertical = 7.dp),
            horizontalArrangement = Arrangement.spacedBy(7.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            val connected = ble.state == BleManager.ConnState.CONNECTED
            Box(
                Modifier
                    .size(9.dp)
                    .background(if (connected) Palette.good else Palette.faint, CircleShape),
            )
            Text(
                if (connected) "OpenTrailPaper" else connectLabel(ble.state),
                style = barlow(14.sp, FontWeight.SemiBold),
                color = Palette.ink,
            )
            if (connected) {
                Text(
                    "${s.battery}%",
                    style = barlow(14.sp, FontWeight.SemiBold),
                    color = Palette.muted,
                )
            }
        }
    }
}

private fun connectLabel(state: BleManager.ConnState) = when (state) {
    BleManager.ConnState.SCANNING -> "Searching…"
    BleManager.ConnState.CONNECTING -> "Connecting…"
    BleManager.ConnState.POWERED_OFF -> "Bluetooth off"
    else -> "Not connected"
}

@Composable
private fun GpsCard(s: DeviceStatus) {
    Card {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Box(
                Modifier
                    .size(38.dp)
                    .background(if (s.gpsFix) Palette.accentWash else Palette.paper, CircleShape),
                contentAlignment = Alignment.Center,
            ) {
                Icon(
                    if (s.gpsFix) Icons.Filled.LocationOn else Icons.Filled.StarBorder,
                    contentDescription = null,
                    tint = if (s.gpsFix) Palette.accent else Palette.muted,
                    modifier = Modifier.size(18.dp),
                )
            }
            Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(
                    if (s.gpsFix) "GPS lock" else "Acquiring GPS",
                    style = barlow(16.sp, FontWeight.SemiBold),
                    color = Palette.ink,
                )
                Text(
                    if (s.gpsFix) {
                        "${s.sats} satellites in view"
                    } else {
                        "${s.sats} satellites · clear view of sky helps"
                    },
                    style = barlow(13.sp),
                    color = Palette.muted,
                )
            }
            Spacer(Modifier.weight(1f))
            Column(horizontalAlignment = Alignment.End) {
                Text(
                    "${s.sats}",
                    style = TypeScale.value(26.sp),
                    color = if (s.gpsFix) Palette.good else Palette.accent,
                )
                TrackedLabel("Sats")
            }
        }
    }
}

@Composable
private fun SpeedCard(s: DeviceStatus, useMiles: Boolean) {
    Card {
        TrackedLabel("Speed")
        Row(verticalAlignment = Alignment.Bottom) {
            Text(
                String.format(Locale.US, "%.1f", Units.speed(s.speedKmh, useMiles)),
                style = TypeScale.hero(72.sp),
                color = Palette.ink,
                maxLines = 1,
            )
            Spacer(Modifier.width(6.dp))
            Text(
                Units.speedLabel(useMiles),
                style = condensed(22.sp, FontWeight.SemiBold),
                color = Palette.muted,
                modifier = Modifier.padding(bottom = 10.dp),
            )
        }
        HorizontalDivider(Modifier.padding(vertical = 6.dp), color = Palette.hairline)
        Row(horizontalArrangement = Arrangement.spacedBy(22.dp)) {
            // Avg and max are a whole-ride roll-up the head unit keeps and the
            // status packet does not carry; the Rides tab is where they live.
            MiniStat("Avg", "—")
            MiniStat("Max", "—")
        }
    }
}

@Composable
private fun MiniStat(label: String, value: String) {
    Row(
        horizontalArrangement = Arrangement.spacedBy(6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        TrackedLabel(label)
        Text(value, style = condensed(17.sp, FontWeight.SemiBold), color = Palette.ink)
    }
}

@Composable
private fun SensorCard(
    label: String,
    value: String?,
    unit: String,
    sensor: BikeSensor?,
    state: BleManager.ConnState,
    modifier: Modifier = Modifier,
) {
    // Subtitle shows the connected sensor's name, else live/searching state.
    val subtitle: String
    val subtitleColor: Color
    when {
        sensor != null -> {
            subtitle = "${sensor.name} · connected"; subtitleColor = Palette.good
        }

        value != null -> {
            subtitle = "Live"; subtitleColor = Palette.good
        }

        else -> {
            subtitle = if (state == BleManager.ConnState.CONNECTED) "Searching…" else "Not connected"
            subtitleColor = Palette.accent
        }
    }
    Card(modifier) {
        TrackedLabel(label)
        Row(verticalAlignment = Alignment.Bottom) {
            Text(value ?: "—", style = TypeScale.value(30.sp), color = Palette.ink)
            Spacer(Modifier.width(4.dp))
            Text(
                unit,
                style = condensed(14.sp, FontWeight.Medium),
                color = Palette.muted,
                modifier = Modifier.padding(bottom = 4.dp),
            )
        }
        Text(subtitle, style = barlow(12.sp, FontWeight.Medium), color = subtitleColor, maxLines = 1)
    }
}

@Composable
private fun RouteCard(s: DeviceStatus, useMiles: Boolean) {
    Card {
        TrackedLabel("Active route")
        Row(verticalAlignment = Alignment.Bottom) {
            Text(
                String.format(Locale.US, "%.1f", Units.distance(s.remainingKm, useMiles)),
                style = TypeScale.value(30.sp),
                color = Palette.ink,
            )
            Spacer(Modifier.width(6.dp))
            Text(
                "${Units.distLabel(useMiles)} remaining",
                style = barlow(14.sp, FontWeight.Medium),
                color = Palette.muted,
                modifier = Modifier.padding(bottom = 4.dp),
            )
        }
    }
}

/**
 * The head unit's own dashboard, and the way in to rearranging it.
 *
 * Here rather than in Settings because it belongs beside the numbers it is about:
 * the fields above are what the device is showing, and this is where you decide
 * which of them deserve the big type. The thumbnail is a real preview of the
 * panel layout, so the card says what it does without being opened.
 */
@Composable
private fun DashboardCard(ble: BleManager, onOpen: () -> Unit) {
    Card(Modifier.clickable(onClick = onOpen)) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            val layout = ble.dashLayout
            if (layout != null && layout.items.isNotEmpty()) {
                DashPreview(layout, Modifier.width(62.dp).height(110.dp))
            } else {
                Box(
                    Modifier
                        .width(62.dp)
                        .height(110.dp)
                        .border(1.dp, Palette.hairline, RoundedCornerShape(4.dp)),
                    contentAlignment = Alignment.Center,
                ) {
                    Icon(Icons.Filled.GridView, contentDescription = null, tint = Palette.faint)
                }
            }
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("Dashboard", style = TypeScale.title, color = Palette.ink)
                Text(
                    if (layout == null) {
                        "Connect to change which fields the device shows"
                    } else {
                        "Reorder and resize the fields on the device"
                    },
                    style = TypeScale.body,
                    color = Palette.muted,
                )
            }
            Icon(Icons.Filled.ChevronRight, contentDescription = null, tint = Palette.faint)
        }
    }
}
