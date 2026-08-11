package com.raemond.opentrailpaper.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Bolt
import androidx.compose.material.icons.filled.Favorite
import androidx.compose.material.icons.filled.Sensors
import androidx.compose.material.icons.filled.Sync
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.ble.BikeSensor
import com.raemond.opentrailpaper.ble.BleManager

/**
 * Scan for, pair, and forget the head unit's cycling sensors from the phone.
 *
 * The device does the actual BLE central work — a heart-rate strap pairs with the
 * bike computer, not with the phone — so this drives it over the sensors
 * characteristic and shows the live connection state it reports back.
 */
@Composable
fun SensorsSheet(ble: BleManager, onDismiss: () -> Unit) {
    DisposableEffect(Unit) {
        ble.refreshSensors()    // show paired sensors immediately…
        ble.startSensorScan()   // …and scan for new ones
        onDispose { ble.stopSensorScan() }
    }

    FullScreenSheet(
        title = "Sensors",
        onDismiss = onDismiss,
        trailing = {
            if (ble.state == BleManager.ConnState.CONNECTED) {
                TextButton(onClick = {
                    if (ble.scanningSensors) ble.stopSensorScan() else ble.startSensorScan()
                }) {
                    Text(
                        if (ble.scanningSensors) "Stop" else "Scan",
                        style = TypeScale.bodyStrong,
                        color = Palette.accent,
                    )
                }
            }
        },
    ) {
        Column(
            Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            if (ble.state != BleManager.ConnState.CONNECTED) {
                Card {
                    Text(
                        "Connect to your OpenTrailPaper to manage sensors.",
                        style = barlow(15.sp),
                        color = Palette.muted,
                    )
                }
                return@Column
            }

            val paired = ble.sensors.filter { it.paired }
            val available = ble.sensors.filter { !it.paired }

            if (paired.isNotEmpty()) {
                TrackedLabel("My sensors")
                paired.forEach { SensorRow(it, ble) }
            }

            Row(verticalAlignment = Alignment.CenterVertically) {
                TrackedLabel(
                    if (ble.scanningSensors) "Scanning for sensors…" else "Available",
                )
                if (ble.scanningSensors) {
                    Spacer(Modifier.size(8.dp))
                    CircularProgressIndicator(Modifier.size(14.dp), color = Palette.accent)
                }
            }
            if (available.isEmpty()) {
                Text(
                    if (ble.scanningSensors) {
                        "Wake your sensor (spin the cranks / touch the strap)."
                    } else {
                        "Tap Scan to search for nearby sensors."
                    },
                    style = barlow(13.sp),
                    color = Palette.muted,
                )
            } else {
                available.forEach { SensorRow(it, ble) }
            }
        }
    }
}

@Composable
private fun SensorRow(s: BikeSensor, ble: BleManager) {
    Card {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Box(
                Modifier
                    .size(38.dp)
                    .background(if (s.connected) Palette.accentWash else Palette.paper, CircleShape),
                contentAlignment = Alignment.Center,
            ) {
                Icon(
                    when {
                        s.kindsMask and 1 != 0 -> Icons.Filled.Favorite
                        s.kindsMask and 2 != 0 -> Icons.Filled.Bolt
                        s.kindsMask and 4 != 0 -> Icons.Filled.Sync
                        else -> Icons.Filled.Sensors
                    },
                    contentDescription = null,
                    tint = if (s.connected) Palette.accent else Palette.muted,
                    modifier = Modifier.size(18.dp),
                )
            }
            Column(Modifier.weight(1f)) {
                Text(
                    s.name,
                    style = barlow(16.sp, FontWeight.SemiBold),
                    color = Palette.ink,
                    maxLines = 1,
                )
                Text(
                    statusText(s),
                    style = barlow(13.sp),
                    color = if (s.connected) Palette.good else Palette.muted,
                )
            }
            if (s.paired) {
                TextButton(onClick = { ble.forgetSensor(s.addr) }) {
                    Text("Forget", style = barlow(14.sp, FontWeight.SemiBold), color = Palette.muted)
                }
            } else {
                TextButton(onClick = { ble.pairSensor(s.addr) }) {
                    Text(
                        "Connect",
                        style = barlow(14.sp, FontWeight.SemiBold),
                        color = Palette.accent,
                    )
                }
            }
        }
    }
}

private fun statusText(s: BikeSensor): String = when {
    s.connected -> "Connected · ${s.kindsText}"
    s.paired -> "Paired · ${if (s.rssi != 0) "in range" else "not connected"}"
    else -> "${s.kindsText} · ${s.rssi} dBm"
}
