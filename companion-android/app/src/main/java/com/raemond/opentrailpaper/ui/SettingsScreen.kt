package com.raemond.opentrailpaper.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Bluetooth
import androidx.compose.material.icons.filled.CalendarMonth
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.ChevronRight
import androidx.compose.material.icons.filled.Description
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.automirrored.filled.HelpOutline
import androidx.compose.material.icons.filled.LocationOn
import androidx.compose.material.icons.filled.Map
import androidx.compose.material.icons.automirrored.filled.OpenInNew
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.ble.PermissionState
import com.raemond.opentrailpaper.data.FirmwareRelease
import kotlinx.coroutines.launch
import java.text.SimpleDateFormat
import java.util.Locale
import java.util.TimeZone

/** Edit device settings and push them over BLE. */
@Composable
fun SettingsScreen(ble: BleManager, host: HostActions, onShowTutorial: () -> Unit) {
    var showSensors by remember { mutableStateOf(false) }
    var showMaps by remember { mutableStateOf(false) }
    var confirmUpdate by remember { mutableStateOf(false) }
    val connected = ble.state == BleManager.ConnState.CONNECTED

    LaunchedEffect(Unit) { FirmwareRelease.check() }

    Column(
        Modifier
            .fillMaxSize()
            .background(Palette.paper)
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        Row(Modifier.fillMaxWidth().padding(top = 8.dp)) {
            Text("Settings", style = TypeScale.screenTitle, color = Palette.ink)
        }

        // Keep the firmware card visible through the reboot/disconnect of an
        // in-progress (or just-finished) update so its status doesn't vanish.
        if (connected || ble.otaInProgress ||
            ble.otaPhase == BleManager.OtaPhase.FAILED ||
            ble.otaPhase == BleManager.OtaPhase.DONE
        ) {
            FirmwareCard(ble) { confirmUpdate = true }
        }

        if (connected) NavCard("Sensors", sensorSummary(ble)) { showSensors = true }

        // Not gated on the connection, unlike sensors: picking an area and
        // fetching OSM works offline, and the Maps screen only needs the link for
        // the upload itself.
        NavCard("Maps", mapsSummary(ble), icon = Icons.Filled.Map) { showMaps = true }

        Card {
            TrackedLabel("Units")
            Spacer(Modifier.size(10.dp))
            Segmented(
                options = listOf("Metric (km)", "Standard (mi)"),
                selected = if (ble.useMiles) 1 else 0,
                onSelect = { ble.updateUseMiles(it == 1) },
            )
        }

        if (connected) {
            Card {
                TrackedLabel("Clock")
                Spacer(Modifier.size(10.dp))
                Segmented(
                    options = listOf("24-hour", "12-hour"),
                    selected = if (ble.clock24h) 0 else 1,
                    onSelect = { ble.updateClock24h(it == 0) },
                )
            }

            Card {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    TrackedLabel("USB drive", Modifier.weight(1f))
                    Switch(
                        checked = ble.usbDrive,
                        onCheckedChange = { ble.updateUsbDrive(it) },
                        colors = SwitchDefaults.colors(
                            checkedThumbColor = Color.White,
                            checkedTrackColor = Palette.accent,
                        ),
                    )
                }
                Text(
                    if (ble.usbDrive) {
                        "The SD mounts on a computer when plugged in. Turn off to keep the SD " +
                            "with the device (record/maps keep working while plugged in for " +
                            "power or serial)."
                    } else {
                        "The SD stays with the device when plugged in. Turn on to copy maps, " +
                            "firmware, or logs from a computer."
                    },
                    style = barlow(12.sp),
                    color = Palette.muted,
                )
            }
        }

        Card {
            TrackedLabel("FTP")
            Stepper(
                value = "${ble.ftpWatts} W",
                onDecrease = { ble.updateFtp((ble.ftpWatts - 5).coerceAtLeast(50)) },
                onIncrease = { ble.updateFtp((ble.ftpWatts + 5).coerceAtMost(500)) },
            )
        }

        Card {
            TrackedLabel("Timezone")
            Stepper(
                value = tzLabel(ble.tzMinutes),
                onDecrease = { ble.updateTz((ble.tzMinutes - 30).coerceAtLeast(-12 * 60)) },
                onIncrease = { ble.updateTz((ble.tzMinutes + 30).coerceAtMost(14 * 60)) },
            )
        }

        Card {
            TrackedLabel("Backlight")
            Spacer(Modifier.size(10.dp))
            Segmented(
                options = listOf("Off", "Low", "Med", "Bright"),
                selected = ble.backlight.coerceIn(0, 3),
                enabled = connected,
                onSelect = { ble.updateBacklight(it) },
            )
        }

        if (connected) DiagnosticsCard(ble)

        PermissionsCard(ble, host)
        NavCard("How it works", "Replay the intro tutorial", icon = Icons.AutoMirrored.Filled.HelpOutline) {
            onShowTutorial()
        }

        Text(
            if (connected) {
                "Settings sync automatically with your OpenTrailPaper, both ways."
            } else {
                "Connect to sync settings with your OpenTrailPaper."
            },
            style = barlow(13.sp),
            color = Palette.muted,
            modifier = Modifier.fillMaxWidth().padding(top = 4.dp),
        )
        Spacer(Modifier.size(8.dp))
    }

    if (showSensors) SensorsSheet(ble) { showSensors = false }
    if (showMaps) MapsSheet(ble) { showMaps = false }
    ble.logFile?.let { file ->
        DiagnosticsSheet(file) { ble.logFile = null }
    }

    if (confirmUpdate) {
        AlertDialog(
            onDismissRequest = { confirmUpdate = false },
            title = { Text("Install firmware ${FirmwareRelease.latest?.tag ?: ""}?") },
            text = {
                Text(
                    "The app downloads this release from GitHub, sends it to your " +
                        "OpenTrailPaper and restarts it. It takes a few minutes — keep the " +
                        "app open and the device close. If anything goes wrong, the device " +
                        "keeps running its current firmware.",
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    confirmUpdate = false
                    ble.startFirmwareUpdate()
                }) { Text("Install") }
            },
            dismissButton = {
                TextButton(onClick = { confirmUpdate = false }) { Text("Cancel") }
            },
            containerColor = Palette.surface,
        )
    }
}

// MARK: cards

@Composable
private fun NavCard(
    title: String,
    summary: String,
    icon: ImageVector? = null,
    onClick: () -> Unit,
) {
    Card(Modifier.clickable(onClick = onClick)) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            if (icon != null) {
                Icon(icon, contentDescription = null, tint = Palette.accent)
                Spacer(Modifier.size(12.dp))
            }
            Column(Modifier.weight(1f)) {
                TrackedLabel(title)
                Text(summary, style = barlow(15.sp, FontWeight.SemiBold), color = Palette.ink)
            }
            Icon(Icons.Filled.ChevronRight, contentDescription = null, tint = Palette.muted)
        }
    }
}

private fun sensorSummary(ble: BleManager): String {
    val connected = ble.sensors.filter { it.connected }
    if (connected.isNotEmpty()) return connected.joinToString(", ") { it.name } + " connected"
    val paired = ble.sensors.filter { it.paired }
    if (paired.isNotEmpty()) return "${paired.size} saved · none connected"
    return "Scan & pair heart rate, power, cadence"
}

/** The cached device tile list means this still reads correctly while
 *  disconnected, rather than claiming the device holds nothing. */
private fun mapsSummary(ble: BleManager): String {
    val n = ble.deviceTileIds.size
    if (n == 0) return "Download map areas to your device"
    return "$n area${if (n == 1) "" else "s"} on device"
}

@Composable
private fun Stepper(value: String, onDecrease: () -> Unit, onIncrease: () -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text(value, style = TypeScale.value(30.sp), color = Palette.ink, modifier = Modifier.weight(1f))
        StepButton("−", onDecrease)
        Spacer(Modifier.size(8.dp))
        StepButton("+", onIncrease)
    }
}

@Composable
private fun StepButton(label: String, onClick: () -> Unit) {
    Box(
        Modifier
            .size(40.dp)
            .background(Palette.paper, RoundedCornerShape(12.dp))
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        Text(label, style = condensed(22.sp, FontWeight.Bold), color = Palette.accent)
    }
}

private fun tzLabel(minutes: Int): String {
    val h = minutes / 60
    val m = kotlin.math.abs(minutes % 60)
    return if (m == 0) String.format(Locale.US, "UTC%+d", h)
    else String.format(Locale.US, "%+d:%02d", h, m)
}

// MARK: firmware

@Composable
private fun FirmwareCard(ble: BleManager, onInstall: () -> Unit) {
    Card {
        TrackedLabel("Firmware")
        Spacer(Modifier.size(10.dp))
        Row(verticalAlignment = Alignment.Top) {
            Column(Modifier.weight(1f)) {
                Text(
                    "Device: ${ble.deviceFirmware.ifEmpty { "…" }}",
                    style = TypeScale.body,
                    color = Palette.ink,
                )
                // Latest comes from GitHub, so it can be ahead of the installed
                // app — shipping firmware no longer means shipping an app build.
                val release = FirmwareRelease.latest
                Text(
                    when {
                        release != null -> "Latest: ${release.tag}"
                        FirmwareRelease.checking -> "Checking GitHub…"
                        else -> FirmwareRelease.error ?: "Latest: unknown"
                    },
                    style = barlow(12.sp),
                    color = Palette.muted,
                )
            }
            if (ble.updateAvailable) {
                Text(
                    "UPDATE",
                    style = barlow(11.sp, FontWeight.Bold),
                    color = Color.White,
                    modifier = Modifier
                        .background(Palette.accent, RoundedCornerShape(50))
                        .padding(horizontal = 8.dp, vertical = 4.dp),
                )
            }
        }
        Spacer(Modifier.size(12.dp))

        val downloadProgress = FirmwareRelease.downloadProgress
        when {
            ble.otaInProgress -> OtaProgress(ble)

            ble.otaPhase == BleManager.OtaPhase.FAILED -> {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Icon(Icons.Filled.Warning, contentDescription = null, tint = Palette.accent)
                    Text(
                        ble.otaMessage ?: "Update failed",
                        style = barlow(12.sp),
                        color = Palette.ink,
                    )
                }
                Spacer(Modifier.size(10.dp))
                PrimaryButton("Try again", icon = Icons.Filled.Refresh) {
                    ble.startFirmwareUpdate()
                }
                Text(
                    "Or copy firmware.bin to the device's SD card and eject it — that path " +
                        "doesn't use Bluetooth.",
                    style = barlow(11.sp),
                    color = Palette.muted,
                )
            }

            ble.otaPhase == BleManager.OtaPhase.DONE -> Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Icon(Icons.Filled.CheckCircle, contentDescription = null, tint = Palette.good)
                Text(
                    ble.otaMessage ?: "Up to date",
                    style = barlow(12.sp),
                    color = Palette.good,
                )
            }

            downloadProgress != null -> {
                Text("Downloading firmware…", style = barlow(12.sp), color = Palette.muted)
                LinearProgressIndicator(
                    progress = { downloadProgress.toFloat() },
                    modifier = Modifier.fillMaxWidth().padding(top = 4.dp),
                    color = Palette.accent,
                    trackColor = Palette.hairline,
                )
            }

            ble.updateAvailable -> PrimaryButton(
                "Install ${FirmwareRelease.latest?.tag}",
                icon = Icons.Filled.Download,
                onClick = onInstall,
            )

            ble.deviceFirmware.isNotEmpty() && FirmwareRelease.latest != null ->
                Text("Up to date.", style = barlow(12.sp), color = Palette.muted)

            FirmwareRelease.latest == null && !FirmwareRelease.checking -> {
                val scope = rememberCoroutineScope()
                TextButton(onClick = { scope.launch { FirmwareRelease.check() } }) {
                    Text("Check for updates", style = TypeScale.bodyStrong, color = Palette.accent)
                }
            }
        }
    }
}

/** Clear, phase-based progress while an OTA is running. */
@Composable
private fun OtaProgress(ble: BleManager) {
    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            if (ble.otaPhase == BleManager.OtaPhase.SENDING) {
                Icon(Icons.Filled.Download, contentDescription = null, tint = Palette.accent)
            } else {
                CircularProgressIndicator(Modifier.size(20.dp), color = Palette.accent)
            }
            Column {
                Text(
                    when (ble.otaPhase) {
                        BleManager.OtaPhase.SENDING -> "Step 1 of 2 · Sending firmware"
                        BleManager.OtaPhase.SAVING -> "Step 1 of 2 · Saving to the device"
                        // The device is still flashing from SD while it verifies
                        // (its screen reads "Installing"), so match that rather
                        // than saying "Verifying".
                        BleManager.OtaPhase.INSTALLING,
                        BleManager.OtaPhase.VERIFYING,
                        -> "Step 2 of 2 · Installing"

                        else -> "Updating"
                    },
                    style = barlow(14.sp, FontWeight.SemiBold),
                    color = Palette.ink,
                )
                Text(ble.otaMessage ?: "", style = barlow(12.sp), color = Palette.muted)
            }
        }
        if (ble.otaPhase == BleManager.OtaPhase.SENDING) {
            LinearProgressIndicator(
                progress = { ble.otaProgress.toFloat() },
                modifier = Modifier.fillMaxWidth(),
                color = Palette.accent,
                trackColor = Palette.hairline,
            )
            Text(
                "${(ble.otaProgress * 100).toInt()}% sent",
                style = barlow(11.sp),
                color = Palette.muted,
            )
        }
        Text(
            when (ble.otaPhase) {
                BleManager.OtaPhase.SENDING, BleManager.OtaPhase.SAVING ->
                    "Keep the app open and the device nearby, and don't lock the phone."

                BleManager.OtaPhase.INSTALLING, BleManager.OtaPhase.VERIFYING ->
                    "The device restarts to install (~30 s) and reconnects on its own. Keep it " +
                        "powered on and close."

                else -> ""
            },
            style = barlow(11.sp),
            color = Palette.muted,
        )
    }
}

// MARK: diagnostics

@Composable
private fun DiagnosticsCard(ble: BleManager) {
    Card {
        TrackedLabel("Diagnostics")
        Text(
            "The device keeps a daily log (boot, GPS, BLE, OTA, errors). Grab today's, or " +
                "pick a specific day.",
            style = barlow(12.sp),
            color = Palette.muted,
            modifier = Modifier.padding(vertical = 8.dp),
        )
        if (ble.downloadingLog) {
            Text(
                "Downloading ${logDayLabel(ble.downloadingName ?: "log")}…",
                style = barlow(12.sp),
                color = Palette.muted,
            )
            LinearProgressIndicator(
                progress = { ble.downloadProgress.toFloat() },
                modifier = Modifier.fillMaxWidth().padding(top = 4.dp),
                color = Palette.accent,
                trackColor = Palette.hairline,
            )
        } else {
            PrimaryButton("Download today's log", icon = Icons.Filled.Description) {
                ble.downloadLog()
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                TextButton(onClick = { ble.requestLogList() }) {
                    Icon(Icons.Filled.CalendarMonth, contentDescription = null, tint = Palette.accent)
                    Spacer(Modifier.size(6.dp))
                    Text("Other days", style = TypeScale.bodyStrong, color = Palette.accent)
                }
                if (ble.loadingLogs) {
                    CircularProgressIndicator(Modifier.size(16.dp), color = Palette.accent)
                }
            }
            ble.deviceLogs.forEach { log ->
                Row(
                    Modifier
                        .fillMaxWidth()
                        .clickable { ble.downloadLogFile(log.name) }
                        .padding(vertical = 8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(logDayLabel(log.name), style = barlow(13.sp), color = Palette.ink)
                    Spacer(Modifier.weight(1f))
                    Text(sizeLabel(log.size), style = barlow(11.sp), color = Palette.muted)
                    Spacer(Modifier.size(8.dp))
                    Icon(Icons.Filled.Download, contentDescription = null, tint = Palette.accent)
                }
                HorizontalDivider(color = Palette.hairline)
            }
        }
    }
}

/** "20260716.log" -> "Jul 16, 2026"; "pending.log" -> "Before first GPS fix". */
private fun logDayLabel(name: String): String {
    val base = name.removeSuffix(".log")
    if (base == "diag") return "Today"
    if (base == "pending") return "Before first GPS fix"
    if (base.length != 8 || base.toIntOrNull() == null) return name
    val parsed = runCatching {
        SimpleDateFormat("yyyyMMdd", Locale.US)
            .apply { timeZone = TimeZone.getTimeZone("UTC") }
            .parse(base)
    }.getOrNull() ?: return name
    return SimpleDateFormat("MMM d, yyyy", Locale.getDefault()).format(parsed)
}

private fun sizeLabel(bytes: Int) = if (bytes >= 1024) "${bytes / 1024} KB" else "$bytes B"

// MARK: permissions

/**
 * Permissions, and how to fix the ones that are missing.
 *
 * Once a permission has been refused twice Android stops showing the dialog at
 * all, so without a route to system Settings a "no" tapped during the tutorial
 * would be permanent and unexplained. Granted ones are listed too, so this reads
 * as the whole picture rather than only a list of complaints.
 */
@Composable
private fun PermissionsCard(ble: BleManager, host: HostActions) {
    Card {
        TrackedLabel("Permissions")
        Spacer(Modifier.size(12.dp))
        PermissionRow(
            icon = Icons.Filled.Bluetooth,
            name = "Bluetooth",
            state = ble.bluetoothPermission,
            granted = if (ble.bluetoothPermission.isGranted && !ble.bluetoothPoweredOn) {
                "Allowed, but Bluetooth is switched off — turn it on in Quick Settings."
            } else {
                "Allowed. This is how the app talks to your OpenTrailPaper."
            },
            missing = "Without it the app can't reach your device at all — no routes, maps, " +
                "settings or ride downloads.",
            onAsk = host.requestBluetooth,
        )
        HorizontalDivider(Modifier.padding(vertical = 12.dp), color = Palette.hairline)
        PermissionRow(
            icon = Icons.Filled.LocationOn,
            name = "Location",
            state = ble.locationPermission,
            granted = "Allowed while you're using the app.",
            missing = "The app still works: you lose your position on the map, the GPS " +
                "warm-start that helps the device lock on faster, and the backup fix when it " +
                "can't see the sky.",
            onAsk = host.requestLocation,
        )

        // Only offered when Settings can actually change something — a restricted
        // permission or a switched-off radio is not fixable there, and offering
        // the button anyway would send the user somewhere that can't help.
        if (ble.bluetoothPermission.fixableInSettings || ble.locationPermission.fixableInSettings) {
            Spacer(Modifier.size(12.dp))
            PrimaryButton("Open Settings", icon = Icons.AutoMirrored.Filled.OpenInNew) {
                host.openAppSettings()
            }
            Text(
                "Opens this app's page in Settings. Changes apply as soon as you come back.",
                style = barlow(11.sp),
                color = Palette.muted,
            )
        }
    }
}

@Composable
private fun PermissionRow(
    icon: ImageVector,
    name: String,
    state: PermissionState,
    granted: String,
    missing: String,
    onAsk: () -> Unit,
) {
    Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        Icon(
            icon,
            contentDescription = null,
            tint = if (state.isGranted) Palette.good else Palette.accent,
            modifier = Modifier.size(24.dp),
        )
        Column(Modifier.weight(1f)) {
            Row(
                horizontalArrangement = Arrangement.spacedBy(6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(name, style = barlow(15.sp, FontWeight.SemiBold), color = Palette.ink)
                Text(
                    when (state) {
                        PermissionState.GRANTED -> "ALLOWED"
                        PermissionState.DENIED -> "NOT ALLOWED"
                        PermissionState.NOT_DETERMINED -> "NOT ASKED"
                        PermissionState.UNAVAILABLE -> "UNAVAILABLE"
                    },
                    style = barlow(10.sp, FontWeight.Bold),
                    color = Color.White,
                    modifier = Modifier
                        .background(
                            if (state.isGranted) Palette.good else Palette.accent,
                            RoundedCornerShape(50),
                        )
                        .padding(horizontal = 7.dp, vertical = 3.dp),
                )
            }
            Text(
                if (state.isGranted) granted else missing,
                style = barlow(12.sp),
                color = Palette.muted,
            )
            // Never asked (skipped during the tutorial): the system will still
            // show the real dialog, so ask here rather than sending the user to
            // Settings for something they were never offered.
            if (state == PermissionState.NOT_DETERMINED) {
                TextButton(onClick = onAsk, contentPadding = androidx.compose.foundation.layout.PaddingValues(0.dp)) {
                    Text(
                        "Allow ${name.lowercase(Locale.US)}",
                        style = barlow(13.sp, FontWeight.SemiBold),
                        color = Palette.accent,
                    )
                }
            }
        }
    }
}
