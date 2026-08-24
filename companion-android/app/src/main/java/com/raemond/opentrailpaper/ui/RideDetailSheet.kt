package com.raemond.opentrailpaper.ui

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
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Share
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.data.BoundingBox
import com.raemond.opentrailpaper.data.RidePreview
import com.raemond.opentrailpaper.data.Units
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Preview of a downloaded ride: the track on a map plus summary stats, with a
 * Share button for the original .fit file.
 */
@Composable
fun RideDetailSheet(
    file: File,
    preview: RidePreview,
    useMiles: Boolean,
    onDismiss: () -> Unit,
) {
    val context = LocalContext.current
    val title = remember(preview.startEpochMs) {
        preview.startEpochMs
            ?.let { SimpleDateFormat("MMM d, HH:mm", Locale.getDefault()).format(Date(it)) }
            ?: "Ride"
    }

    FullScreenSheet(title = title, onDismiss = onDismiss) {
        Column(
            Modifier
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Box(
                Modifier
                    .fillMaxWidth()
                    .height(300.dp)
                    .clip(RoundedCornerShape(18.dp))
                    .border(1.dp, Palette.hairline, RoundedCornerShape(18.dp))
                    .background(Palette.paper),
            ) {
                val bounds = remember(preview) { BoundingBox.around(preview.coordinates) }
                OsmMap(
                    modifier = Modifier.fillMaxSize(),
                    route = preview.coordinates,
                    camera = remember(bounds) {
                        bounds?.let { MapCamera.box(it, paddingPx = 64) }
                    },
                )
            }

            // Mirrors the device's ride-complete screen: distance hero, then
            // moving time / avg speed / avg + normalized power / avg HR / DEM
            // ascent, read straight from the FIT session.
            Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                Stat(
                    "Distance",
                    String.format(
                        Locale.US, "%.1f",
                        Units.distance(preview.distanceKm, useMiles),
                    ),
                    Units.distLabel(useMiles),
                    big = true,
                    modifier = Modifier.weight(1f),
                )
                Stat("Moving Time", hms(preview.movingTime), "", modifier = Modifier.weight(1f))
            }
            Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                Stat(
                    "Avg Speed",
                    String.format(
                        Locale.US, "%.1f",
                        Units.speed(preview.avgSpeedKmh, useMiles),
                    ),
                    Units.speedLabel(useMiles),
                    modifier = Modifier.weight(1f),
                )
                Stat(
                    "Avg Power",
                    preview.avgPower?.toString() ?: "—",
                    "W",
                    modifier = Modifier.weight(1f),
                )
            }
            Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                Stat(
                    "Norm Power",
                    preview.normPower?.toString() ?: "—",
                    "W",
                    modifier = Modifier.weight(1f),
                )
                Stat(
                    "Avg HR",
                    preview.avgHeartRate?.toString() ?: "—",
                    "bpm",
                    modifier = Modifier.weight(1f),
                )
            }
            Row(horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                Stat(
                    "Ascent",
                    String.format(
                        Locale.US, "%.0f",
                        Units.elevation(preview.ascentM, useMiles),
                    ),
                    Units.elevLabel(useMiles),
                    modifier = Modifier.weight(1f),
                )
                Spacer(Modifier.weight(1f))
            }

            PrimaryButton("Share .fit file", icon = Icons.Filled.Share) {
                Share.fit(context, file)
            }
        }
    }
}

@Composable
fun Stat(
    label: String,
    value: String,
    unit: String,
    modifier: Modifier = Modifier,
    big: Boolean = false,
) {
    Card(modifier) {
        TrackedLabel(label)
        Row(verticalAlignment = Alignment.Bottom) {
            Text(
                value,
                style = if (big) TypeScale.hero(52.sp) else TypeScale.value(),
                color = Palette.ink,
                maxLines = 1,
            )
            if (unit.isNotEmpty()) {
                Spacer(Modifier.size(4.dp))
                Text(
                    unit,
                    style = TypeScale.label,
                    color = Palette.muted,
                    modifier = Modifier.padding(bottom = 4.dp),
                )
            }
        }
    }
}

/**
 * Shown when a downloaded ride can't be decoded — lets the user share the raw
 * .fit so it can be inspected.
 */
@Composable
fun RideParseErrorSheet(
    file: File,
    canRetry: Boolean = false,
    onRetry: () -> Unit = {},
    onDismiss: () -> Unit,
) {
    val context = LocalContext.current
    FullScreenSheet(title = "Ride", onDismiss = onDismiss, confirmLabel = "Close") {
        Column(
            Modifier.fillMaxSize().padding(28.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(20.dp),
        ) {
            Spacer(Modifier.height(40.dp))
            Icon(
                Icons.Filled.Warning,
                contentDescription = null,
                tint = Palette.accent,
                modifier = Modifier.size(44.dp),
            )
            Text("Couldn't read this ride", style = TypeScale.title, color = Palette.ink)
            Text(
                if (canRetry) {
                    "The app couldn't parse this file. Download it again from the device, " +
                        "or share the raw .fit so it can be analyzed."
                } else {
                    "The full file downloaded but the app couldn't parse it. Share the raw " +
                        ".fit file so it can be analyzed."
                },
                style = TypeScale.body,
                color = Palette.muted,
                textAlign = TextAlign.Center,
            )
            if (canRetry) {
                PrimaryButton("Download again", icon = Icons.Filled.Refresh, onClick = onRetry)
                TextButton(onClick = { Share.fit(context, file) }) {
                    Text("Share .fit file", color = Palette.ink, style = barlow(14.sp))
                }
            } else {
                PrimaryButton("Share .fit file", icon = Icons.Filled.Share) {
                    Share.fit(context, file)
                }
            }
            TextButton(onClick = onDismiss) {
                Text("Close", color = Palette.muted, style = barlow(14.sp))
            }
        }
    }
}

private fun hms(seconds: Double): String {
    val s = seconds.toInt().coerceAtLeast(0)
    return String.format(Locale.US, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60)
}
