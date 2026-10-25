package com.raemond.opentrailpaper.ui

import com.raemond.opentrailpaper.data.UploadLog
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
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
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Share
import androidx.compose.material.icons.filled.Warning
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.painterResource
import androidx.compose.foundation.layout.width
import com.raemond.opentrailpaper.R
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.raemond.opentrailpaper.data.BoundingBox
import com.raemond.opentrailpaper.data.RidePreview
import com.raemond.opentrailpaper.data.SyncAccounts
import com.raemond.opentrailpaper.data.Units
import java.io.File
import kotlinx.coroutines.launch
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

    // "Sunday morning ride" and "JUL 19 · 7:15", as the Rides list names it.
    val heading = remember(preview.startEpochMs) { preview.startEpochMs?.let { rideHeading(it) } }
    BackHeaderSheet(
        title = heading?.first ?: "Ride",
        subtitle = heading?.second,
        backLabel = "Back to rides",
        onDismiss = onDismiss,
    ) {
        // Fits on one screen: the map takes whatever height the stats and
        // upload grid leave, and the page only scrolls once even a short map
        // doesn't fit (a long list of connected accounts).
        BoxWithConstraints(Modifier.fillMaxSize()) {
            val density = LocalDensity.current
            var restHeight by remember { mutableStateOf(0.dp) }
            val mapHeight = maxOf(180.dp, maxHeight - restHeight - 16.dp - 32.dp)
            Column(
                Modifier
                    .verticalScroll(rememberScrollState())
                    .padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                Box(
                    Modifier
                        .fillMaxWidth()
                        .height(mapHeight)
                        .background(Palette.paper)
                        .einkBorder()
                        .padding(Rule.border),
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

                Column(
                    Modifier.onSizeChanged { restHeight = with(density) { it.height.toDp() } },
                    verticalArrangement = Arrangement.spacedBy(16.dp),
                ) {
                    StatsCard(preview, useMiles)

                    // Straight to a connected account (Settings > Accounts); the share
                    // button below stays for everything else.
                    UploadButtons(file, title)
                    SecondaryButton("Share .fit file", icon = Icons.Filled.Share) {
                        Share.fit(context, file)
                    }
                }
            }
        }
    }
}

/** "Sunday morning ride" and "Jul 19 · 07:15" for a ride that started at [epochMs]. */
private fun rideHeading(epochMs: Long): Pair<String, String> {
    val d = Date(epochMs)
    val cal = java.util.Calendar.getInstance().apply { time = d }
    val weekday = SimpleDateFormat("EEEE", Locale.getDefault()).format(d)
    val h = cal.get(java.util.Calendar.HOUR_OF_DAY)
    val part = when {
        h < 5 -> "night"
        h < 12 -> "morning"
        h < 17 -> "afternoon"
        h < 21 -> "evening"
        else -> "night"
    }
    return "$weekday $part ride" to SimpleDateFormat("MMM d · HH:mm", Locale.getDefault()).format(d)
}

/**
 * Mirrors the device's ride-complete screen in one box: distance, moving time
 * and DEM ascent, then avg speed / avg + normalized power / avg HR, read
 * straight from the FIT session. 2dp rules between cells.
 */
@Composable
private fun StatsCard(preview: RidePreview, useMiles: Boolean) {
    Column(Modifier.fillMaxWidth().einkBorder()) {
        StatRow(
            listOf(
                StatCell(
                    "Distance",
                    String.format(Locale.US, "%.1f", Units.distance(preview.distanceKm, useMiles)),
                    Units.distLabel(useMiles), 38.sp,
                ),
                StatCell("Moving", hms(preview.movingTime), "", 26.sp),
                StatCell(
                    "Ascent",
                    String.format(Locale.US, "%.0f", Units.elevation(preview.ascentM, useMiles)),
                    Units.elevLabel(useMiles), 26.sp,
                ),
            ),
            small = false,
        )
        Box(Modifier.fillMaxWidth().height(Rule.divider).background(Palette.ink))
        StatRow(
            listOf(
                StatCell("Avg spd", String.format(Locale.US, "%.1f", Units.speed(preview.avgSpeedKmh, useMiles)), ""),
                StatCell("Avg pwr", preview.avgPower?.toString() ?: "--", ""),
                StatCell("NP", preview.normPower?.toString() ?: "--", ""),
                StatCell("Avg HR", preview.avgHeartRate?.toString() ?: "--", ""),
            ),
            small = true,
        )
    }
}

private data class StatCell(val label: String, val value: String, val unit: String, val size: TextUnit = 22.sp)

/** One row of the stats box: cells split by 2dp rules, label over value. */
@Composable
private fun StatRow(cells: List<StatCell>, small: Boolean) {
    Row(Modifier.fillMaxWidth().height(IntrinsicSize.Min)) {
        cells.forEachIndexed { i, c ->
            if (i > 0) Box(Modifier.width(Rule.divider).fillMaxHeight().background(Palette.ink))
            Column(
                Modifier
                    .weight(1f)
                    .padding(horizontal = if (small) 10.dp else 12.dp, vertical = 8.dp),
            ) {
                Text(
                    c.label.uppercase(),
                    style = caps(if (small) 10.sp else 11.sp, if (small) 1.5.sp else 2.sp),
                    color = Palette.ink, maxLines = 1, softWrap = false,
                )
                Row(verticalAlignment = Alignment.Bottom) {
                    Text(
                        c.value, style = anton(c.size), color = Palette.ink, maxLines = 1, softWrap = false,
                        modifier = Modifier.alignByBaseline(),
                    )
                    if (c.unit.isNotEmpty()) {
                        Spacer(Modifier.size(3.dp))
                        Text(
                            c.unit.uppercase(), style = caps(if (small) 11.sp else 12.sp, 0.sp), color = Palette.ink,
                            modifier = Modifier.alignByBaseline(),
                        )
                    }
                }
            }
        }
    }
}

/** One tile per connected account, side by side; tap a tile to upload. */
@Composable
private fun UploadButtons(file: File, name: String) {
    val scope = rememberCoroutineScope()
    var uploading by remember { mutableStateOf<SyncAccounts.Provider?>(null) }
    val status = remember { mutableStateMapOf<SyncAccounts.Provider, String>() }
    val context = LocalContext.current
    // Uploaded earlier from this phone: show the tile as done.
    LaunchedEffect(file.name) {
        for (p in UploadLog.providers(context, file.name)) if (status[p] == null) status[p] = "Uploaded"
    }
    val connected = SyncAccounts.Provider.entries.filter { SyncAccounts.isConnected(it) }
    if (connected.isEmpty()) return
    fun uploaded(p: SyncAccounts.Provider) = status[p]?.startsWith("Uploaded") == true
    fun failed(p: SyncAccounts.Provider) = status[p] != null && !uploaded(p)

    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        TrackedLabel("Upload to")
        // At most three across; with more connected the rest wrap to a new row.
        val columns = minOf(connected.size, 3)
        for (row in connected.chunked(columns)) {
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                for (p in row) {
                    UploadTile(
                        p,
                        state = when {
                            uploading == p -> "Uploading"
                            uploaded(p) -> "Uploaded"
                            failed(p) -> "Try again"
                            else -> "Upload"
                        },
                        done = uploaded(p),
                        failed = failed(p),
                        enabled = uploading == null && !uploaded(p),
                        dimmed = uploading != null && uploading != p,
                        modifier = Modifier.weight(1f),
                    ) {
                        uploading = p
                        status.remove(p)
                        scope.launch {
                            status[p] = try {
                                SyncAccounts.upload(file, p, name)
                            } catch (e: Exception) {
                                e.message ?: "Upload failed."
                            }
                            if (uploaded(p)) UploadLog.record(context, p, file.name)
                            uploading = null
                        }
                    }
                }
                repeat(columns - row.size) { Spacer(Modifier.weight(1f)) }
            }
        }
        // Full messages go under the grid; a tile only has room for a word.
        for (p in connected.filter { failed(it) }) {
            val s = status[p].orEmpty()
            Text(if (p.title in s) s else "${p.title}: $s", style = barlow(12.sp, FontWeight.SemiBold), color = Palette.ink)
        }
    }
}

@Composable
private fun UploadTile(
    p: SyncAccounts.Provider,
    state: String,
    done: Boolean,
    failed: Boolean,
    enabled: Boolean,
    dimmed: Boolean,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    // A finished upload turns its tile black; one in flight or failed is hatched.
    val fg = if (done) Palette.paper else Palette.ink
    Column(
        modifier = modifier
            .alpha(if (dimmed) 0.5f else 1f)
            .then(
                when {
                    done -> Modifier.background(Palette.ink)
                    state == "Uploading" || failed -> Modifier.hatched()
                    else -> Modifier.background(Palette.paper)
                },
            )
            .einkBorder()
            .clickable(enabled = enabled, onClickLabel = "Upload to ${p.title}", onClick = onClick)
            .padding(vertical = 10.dp, horizontal = 6.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        ProviderLogo(p, 40.dp)
        Text(p.title, style = barlow(13.sp, FontWeight.Bold), color = fg, maxLines = 1)
        Text(
            when (state) {
                "Uploading" -> "UPLOADING…"
                "Uploaded" -> "✓ UPLOADED"
                "Try again" -> "TRY AGAIN"
                else -> "↑ UPLOAD"
            },
            style = caps(11.sp, 1.5.sp), color = fg, maxLines = 1,
        )
    }
}

/**
 * The service's logo as a square tile. Ride with GPS and
 * intervals.icu use their published marks as-is (Ride with GPS asks for its
 * colours to be left alone) on a light tile; Strava's chevrons sit on its orange.
 */
@Composable
fun ProviderLogo(p: SyncAccounts.Provider, size: Dp) {
    val strava = p == SyncAccounts.Provider.STRAVA
    Box(
        Modifier.size(size)
            .background(if (strava) Palette.strava else Color.White)
            .then(if (strava) Modifier else Modifier.border(Rule.border, Palette.ink)),
        contentAlignment = Alignment.Center,
    ) {
        when (p) {
            SyncAccounts.Provider.STRAVA -> Canvas(Modifier.size(size * 0.56f)) {
                val k = this.size.minDimension / 24f
                fun Path.poly(vararg xy: Float) {
                    moveTo(xy[0] * k, xy[1] * k)
                    for (i in 2 until xy.size step 2) lineTo(xy[i] * k, xy[i + 1] * k)
                    close()
                }
                // Strava's two chevrons, on a 24-unit grid.
                drawPath(
                    Path().apply {
                        poly(15.387f, 17.944f, 13.298f, 13.828f, 10.233f, 13.828f,
                            15.387f, 24f, 20.537f, 13.828f, 17.471f, 13.828f)
                        poly(10.463f, 8.229f, 13.299f, 13.827f, 17.471f, 13.827f,
                            10.463f, 0f, 3.463f, 13.828f, 7.632f, 13.828f)
                    },
                    Color.White,
                )
            }
            SyncAccounts.Provider.INTERVALS -> Image(
                painterResource(R.drawable.logo_intervals), null, Modifier.width(size * 0.72f),
                contentScale = ContentScale.FillWidth,
            )
            SyncAccounts.Provider.RIDEWITHGPS -> Image(
                painterResource(R.drawable.logo_rwgps), null, Modifier.width(size * 0.74f),
                contentScale = ContentScale.FillWidth,
            )
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
                tint = Palette.ink,
                modifier = Modifier.size(44.dp),
            )
            Text("COULDN'T READ THIS RIDE", style = TypeScale.title, color = Palette.ink)
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
