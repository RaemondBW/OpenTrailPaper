package com.raemond.opentrailpaper.ui

import android.graphics.Bitmap
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.combinedClickable
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
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.FiberManualRecord
import androidx.compose.material.icons.filled.Map
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Schedule
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import com.raemond.opentrailpaper.ble.BleManager
import com.raemond.opentrailpaper.ble.RideFile
import com.raemond.opentrailpaper.data.FitDecoder
import com.raemond.opentrailpaper.data.LatLon
import com.raemond.opentrailpaper.data.RidePreview
import com.raemond.opentrailpaper.data.Units
import com.raemond.opentrailpaper.map.MapSnapshotter
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.text.SimpleDateFormat
import java.util.Calendar
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import kotlin.math.cos
import kotlin.math.max

/**
 * Lists recorded rides on the device and downloads a selected .fit file over
 * BLE, then offers the system share sheet (Strava, Files, mail).
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
fun RidesScreen(ble: BleManager) {
    val previews = remember { mutableStateMapOf<String, RidePreview>() }
    val loading = remember { mutableStateMapOf<String, Boolean>() }
    var detail by remember { mutableStateOf<Pair<java.io.File, RidePreview>?>(null) }
    var failedFile by remember { mutableStateOf<java.io.File?>(null) }
    var cachedNames by remember { mutableStateOf(ble.cachedRides()) }

    // Always show the pre-synced (locally cached) rides; when connected, merge in
    // the device's live list too. Mid-ride the device refuses to list the SD (it's
    // busy recording), so the device list is empty then — the cached rides keep
    // showing instead of the view going blank. Newest first.
    val rides: List<RideFile> = remember(ble.rides, cachedNames, ble.state) {
        if (ble.state != BleManager.ConnState.CONNECTED) {
            cachedNames
        } else {
            val seen = HashSet<String>()
            (ble.rides + cachedNames).filter { seen.add(it.name) }
                .sortedByDescending { it.name }
        }
    }

    LaunchedEffect(ble.state) {
        if (ble.state == BleManager.ConnState.CONNECTED) ble.refreshRides()
    }

    // A finished download is both a new cache entry and, on iOS, the moment the
    // detail sheet opens — decode once and do both.
    LaunchedEffect(ble.downloadedFile) {
        val file = ble.downloadedFile ?: return@LaunchedEffect
        val decoded = withContext(Dispatchers.Default) {
            runCatching { FitDecoder.decode(file.readBytes()) }.getOrNull()
        }
        cachedNames = ble.cachedRides()
        // A queued ride that lands while the rider is looking at an earlier one
        // fills in its row rather than yanking the sheet out from under them.
        val busy = detail != null || failedFile != null
        if (decoded != null) {
            previews[file.name] = decoded
            if (!busy) detail = file to decoded
        } else if (!busy) {
            failedFile = file
        }
    }

    Column(
        Modifier
            .fillMaxSize()
            .background(Palette.paper)
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        RidesHeader(ble)
        if (ble.status.recording) InProgressBanner()
        if (rides.isNotEmpty()) WeekSummary(rides, previews, ble.useMiles)

        when {
            ble.state == BleManager.ConnState.CONNECTED && ble.loadingRides ->
                Box(Modifier.fillMaxWidth().padding(top = 30.dp), Alignment.Center) {
                    CircularProgressIndicator(color = Palette.accent)
                }

            rides.isEmpty() -> Card {
                Text(
                    when {
                        ble.status.recording -> "No synced rides yet"
                        ble.state == BleManager.ConnState.CONNECTED -> "No rides found"
                        else -> "No cached rides"
                    },
                    style = TypeScale.title,
                    color = Palette.ink,
                )
                Text(
                    when {
                        ble.status.recording ->
                            "The current ride is being recorded to the SD card — it'll appear " +
                                "here once you finish and stop it."

                        ble.state == BleManager.ConnState.CONNECTED ->
                            "Rides are saved to the SD card and sync here automatically."

                        else ->
                            "Connect to your OpenTrailPaper to download rides. Downloaded rides " +
                                "stay here for offline viewing."
                    },
                    style = barlow(14.sp),
                    color = Palette.muted,
                )
            }

            else -> {
                rides.forEach { ride ->
                    val cached = ble.isCached(ride.name)
                    // Lazily decode cached rides off the main thread so the list
                    // shows thumbnails and stats without re-parsing every frame.
                    LaunchedEffect(ride.name, cached) {
                        if (!cached || previews.containsKey(ride.name)) return@LaunchedEffect
                        if (loading.put(ride.name, true) == true) return@LaunchedEffect
                        val decoded = withContext(Dispatchers.Default) {
                            runCatching {
                                FitDecoder.decode(ble.cachedRideFile(ride.name).readBytes())
                            }.getOrNull()
                        }
                        loading.remove(ride.name)
                        if (decoded != null) previews[ride.name] = decoded
                    }

                    RideRow(
                        ride = ride,
                        cached = cached,
                        preview = previews[ride.name],
                        downloading = ble.downloadingName == ride.name,
                        queued = ride.name in ble.queuedDownloads,
                        progress = ble.downloadProgress,
                        useMiles = ble.useMiles,
                        onTap = {
                            val file = ble.cachedRideFile(ride.name)
                            val p = previews[ride.name]
                            when {
                                p != null -> detail = file to p
                                file.exists() -> failedFile = file
                                else -> ble.downloadRide(ride.name)
                            }
                        },
                        onDelete = {
                            ble.deleteRide(ride.name)
                            previews.remove(ride.name)
                            cachedNames = ble.cachedRides()
                        },
                    )
                }
                Text(
                    "Tap to preview & share · long-press to delete",
                    style = barlow(12.sp),
                    color = Palette.muted,
                )
            }
        }
    }

    detail?.let { (file, preview) ->
        RideDetailSheet(file, preview, ble.useMiles) { detail = null }
    }
    failedFile?.let { file ->
        RideParseErrorSheet(
            file = file,
            canRetry = ble.state == BleManager.ConnState.CONNECTED,
            onRetry = {
                // Throw away a cached file that won't parse and fetch it again —
                // the way out for rides left corrupt by an interrupted transfer.
                failedFile = null
                file.delete()
                previews.remove(file.name)
                cachedNames = ble.cachedRides()
                ble.downloadRide(file.name)
            },
        ) { failedFile = null }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun RidesHeader(ble: BleManager) {
    Row(
        Modifier.fillMaxWidth().padding(top = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text("Rides", style = TypeScale.screenTitle, color = Palette.ink)
        Spacer(Modifier.weight(1f))
        Text(
            when {
                ble.state != BleManager.ConnState.CONNECTED -> "Offline"
                ble.loadingRides -> "Syncing…"
                else -> "Synced"
            },
            style = barlow(13.sp, FontWeight.Medium),
            color = Palette.muted,
        )
        Spacer(Modifier.size(8.dp))
        val connected = ble.state == BleManager.ConnState.CONNECTED
        Box(
            Modifier
                .size(34.dp)
                .background(Palette.surface, CircleShape)
                .border(1.dp, Palette.hairline, CircleShape)
                .combinedClickable(onClick = { if (connected) ble.refreshRides() else ble.startScan() }),
            contentAlignment = Alignment.Center,
        ) {
            Icon(
                if (connected) Icons.Filled.Refresh else Icons.Filled.Wifi,
                contentDescription = if (connected) "Refresh" else "Scan",
                tint = Palette.accent,
                modifier = Modifier.size(16.dp),
            )
        }
    }
}

/** Shown while the device is actively recording, so the tab makes clear a ride is
 *  underway (and why new rides haven't synced yet). */
@Composable
private fun InProgressBanner() {
    Card {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Icon(
                Icons.Filled.FiberManualRecord,
                contentDescription = null,
                tint = androidx.compose.ui.graphics.Color.Red,
            )
            Column {
                Text(
                    "Ride in progress",
                    style = barlow(15.sp, FontWeight.SemiBold),
                    color = Palette.ink,
                )
                Text(
                    "Recording to the SD card — it'll sync here when you stop.",
                    style = barlow(12.sp),
                    color = Palette.muted,
                )
            }
        }
    }
}

/** Week roll-up from whatever previews are decoded (fills in as rows load). */
@Composable
private fun WeekSummary(
    rides: List<RideFile>,
    previews: Map<String, RidePreview>,
    useMiles: Boolean,
) {
    val recent = rides.filter { daysAgo(it.name) <= 7 }
    val loaded = recent.mapNotNull { previews[it.name] }
    val dist = loaded.sumOf { it.distanceKm }
    val time = loaded.sumOf { it.duration }
    Card {
        Row(Modifier.fillMaxWidth()) {
            SummaryCell("${recent.size}", "rides this week", Modifier.weight(1f))
            SummaryCell(
                String.format(Locale.US, "%.1f", Units.distance(dist, useMiles)),
                "${Units.distLabel(useMiles)} distance",
                Modifier.weight(1f),
            )
            SummaryCell(shortTime(time), "riding time", Modifier.weight(1f))
        }
    }
}

@Composable
private fun SummaryCell(value: String, label: String, modifier: Modifier = Modifier) {
    Column(modifier) {
        Text(value, style = TypeScale.value(26.sp), color = Palette.ink)
        Text(label, style = barlow(12.sp), color = Palette.muted, maxLines = 1)
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun RideRow(
    ride: RideFile,
    cached: Boolean,
    preview: RidePreview?,
    downloading: Boolean,
    queued: Boolean,
    progress: Double,
    useMiles: Boolean,
    onTap: () -> Unit,
    onDelete: () -> Unit,
) {
    Card(Modifier.combinedClickable(onClick = onTap, onLongClick = onDelete)) {
        RideBanner(ride, cached, preview, downloading, queued, progress)
        Spacer(Modifier.size(12.dp))
        Row(verticalAlignment = Alignment.Top) {
            Column(Modifier.weight(1f)) {
                Row(
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                    verticalAlignment = Alignment.Bottom,
                ) {
                    Text(
                        rideName(ride.name),
                        style = condensed(20.sp, FontWeight.SemiBold),
                        color = Palette.ink,
                    )
                    Text(dateSubtitle(ride.name), style = barlow(13.sp), color = Palette.muted)
                }
                if (preview != null) {
                    Row(
                        Modifier.padding(top = 6.dp),
                        horizontalArrangement = Arrangement.spacedBy(14.dp),
                    ) {
                        StatItem(
                            String.format(
                                Locale.US,
                                "%.1f %s",
                                Units.distance(preview.distanceKm, useMiles),
                                Units.distLabel(useMiles),
                            ),
                            "Distance",
                        )
                        StatItem(shortTime(preview.duration), "Time")
                        StatItem(
                            String.format(
                                Locale.US,
                                "%.1f",
                                Units.speed(preview.avgSpeedKmh, useMiles),
                            ),
                            "${Units.speedLabel(useMiles)} avg",
                        )
                        preview.avgPower?.let { StatItem("$it W", "Power") }
                    }
                } else {
                    Text(
                        String.format(
                            Locale.US,
                            "%.0f KB · not downloaded",
                            ride.size / 1024.0,
                        ),
                        style = barlow(12.sp),
                        color = Palette.muted,
                        modifier = Modifier.padding(top = 4.dp),
                    )
                }
            }
            if (queued) {
                Icon(Icons.Filled.Schedule, contentDescription = null, tint = Palette.muted)
            } else if (!cached && !downloading) {
                Icon(Icons.Filled.Download, contentDescription = null, tint = Palette.accent)
            }
        }
    }
}

@Composable
private fun StatItem(value: String, label: String) {
    Column {
        Text(value, style = condensed(16.sp, FontWeight.SemiBold), color = Palette.ink)
        Text(label, style = barlow(10.sp), color = Palette.muted)
    }
}

/**
 * Large map banner: a real map snapshot with the route drawn on it once the ride
 * is decoded, otherwise a prompt to download.
 */
@Composable
private fun RideBanner(
    ride: RideFile,
    cached: Boolean,
    preview: RidePreview?,
    downloading: Boolean,
    queued: Boolean,
    progress: Double,
) {
    Box(
        Modifier
            .fillMaxWidth()
            .height(170.dp)
            .clip(RoundedCornerShape(12.dp))
            .background(Palette.paper)
            .border(1.dp, Palette.hairline, RoundedCornerShape(12.dp)),
        contentAlignment = Alignment.Center,
    ) {
        val coords = preview?.coordinates
        when {
            coords != null && coords.size > 1 -> RouteMapThumbnail(ride.name, coords)
            downloading -> Column(horizontalAlignment = Alignment.CenterHorizontally) {
                LinearProgressIndicator(
                    progress = { progress.toFloat() },
                    modifier = Modifier.size(width = 160.dp, height = 4.dp),
                    color = Palette.accent,
                    trackColor = Palette.hairline,
                )
                Spacer(Modifier.size(8.dp))
                Text("Downloading…", style = barlow(12.sp), color = Palette.muted)
            }

            queued -> Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Icon(
                    Icons.Filled.Schedule,
                    contentDescription = null,
                    tint = Palette.muted,
                    modifier = Modifier.size(30.dp),
                )
                Spacer(Modifier.size(8.dp))
                Text(
                    "Queued — starts after the current download",
                    style = barlow(12.sp),
                    color = Palette.muted,
                    textAlign = TextAlign.Center,
                    modifier = Modifier.padding(horizontal = 16.dp),
                )
            }

            else -> Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Icon(
                    if (cached) Icons.Filled.Map else Icons.Filled.Download,
                    contentDescription = null,
                    tint = Palette.muted,
                    modifier = Modifier.size(30.dp),
                )
                Spacer(Modifier.size(8.dp))
                Text(
                    if (cached) "Loading map…" else "Tap to download",
                    style = barlow(12.sp),
                    color = Palette.muted,
                )
            }
        }
    }
}

/**
 * A real map with the ride's route drawn on top, rendered once and cached by ride
 * name. Falls back to a track-shape glyph while the snapshot loads or if it can't
 * be produced (offline).
 */
@Composable
fun RouteMapThumbnail(name: String, coords: List<LatLon>) {
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val density = LocalDensity.current
        val wPx = with(density) { maxWidth.roundToPx() }
        val hPx = with(density) { maxHeight.roundToPx() }
        // Keyed on the size as well as the ride: a cached bitmap rendered for a
        // different width would be resampled, and a map resampled is a map with
        // unreadable street lines.
        var bitmap by remember(name, wPx, hPx) { mutableStateOf<Bitmap?>(null) }
        LaunchedEffect(name, wPx, hPx) {
            if (wPx > 0 && hPx > 0) bitmap = MapSnapshotter.snapshot(name, coords, wPx, hPx)
        }
        val img = bitmap
        if (img != null) {
            Image(
                img.asImageBitmap(),
                contentDescription = null,
                modifier = Modifier.fillMaxSize(),
                contentScale = ContentScale.Crop,
            )
        } else {
            RouteThumbnail(coords, Modifier.fillMaxSize().padding(10.dp))
        }
    }
}

/**
 * Draws the GPS track as a normalized polyline — a recognizable "shape of the
 * ride" glyph, used as an instant/offline fallback for the map snapshot.
 */
@Composable
fun RouteThumbnail(coords: List<LatLon>, modifier: Modifier = Modifier) {
    Canvas(modifier) {
        if (coords.size < 2) return@Canvas
        val minLat = coords.minOf { it.lat }
        val maxLat = coords.maxOf { it.lat }
        val minLon = coords.minOf { it.lon }
        val maxLon = coords.maxOf { it.lon }
        val meanLat = (minLat + maxLat) / 2
        val cosLat = max(0.01, cos(meanLat * Math.PI / 180))
        val w = (maxLon - minLon) * cosLat
        val h = maxLat - minLat
        if (w <= 0 && h <= 0) return@Canvas
        // Fit the track into the box, preserving aspect ratio, north up.
        val scale = minOf(
            if (w > 0) size.width / w else Double.MAX_VALUE,
            if (h > 0) size.height / h else Double.MAX_VALUE,
        )
        val offX = (size.width - w * scale) / 2
        val offY = (size.height - h * scale) / 2
        fun pt(c: LatLon) = Offset(
            (offX + (c.lon - minLon) * cosLat * scale).toFloat(),
            (size.height - (offY + (c.lat - minLat) * scale)).toFloat(),
        )
        val path = Path()
        path.moveTo(pt(coords[0]).x, pt(coords[0]).y)
        for (c in coords.drop(1)) path.lineTo(pt(c).x, pt(c).y)
        drawPath(
            path,
            Palette.accent,
            style = Stroke(width = 2f, cap = StrokeCap.Round, join = StrokeJoin.Round),
        )
        drawCircle(Palette.good, radius = 2.5f, center = pt(coords[0]))
    }
}

// MARK: naming

/**
 * Filenames look like 20260712-150300.fit — the device names them in UTC, so
 * parse as UTC to get the correct absolute instant. Display then uses the phone's
 * local time zone.
 */
fun rideDate(name: String): Date? {
    val base = name.removeSuffix(".fit")
    return runCatching {
        SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US)
            .apply { timeZone = TimeZone.getTimeZone("UTC") }
            .parse(base)
    }.getOrNull()
}

private fun daysAgo(name: String): Int {
    val d = rideDate(name) ?: return 9999
    return ((System.currentTimeMillis() - d.time) / 86_400_000L).toInt()
}

private fun shortTime(seconds: Double): String {
    val m = (seconds / 60).toInt()
    val h = m / 60
    return if (h > 0) "${h}h ${m % 60}m" else "${m}m"
}

/** Human name like "Saturday afternoon ride" from the file's timestamp. */
private fun rideName(fileName: String): String {
    val d = rideDate(fileName) ?: return fileName
    val cal = Calendar.getInstance().apply { time = d }
    val weekday = SimpleDateFormat("EEEE", Locale.getDefault()).format(d)
    val h = cal.get(Calendar.HOUR_OF_DAY)
    val part = when {
        h < 5 -> "night"
        h < 12 -> "morning"
        h < 17 -> "afternoon"
        h < 21 -> "evening"
        else -> "night"
    }
    return "$weekday $part ride"
}

private fun dateSubtitle(fileName: String): String {
    val d = rideDate(fileName) ?: return ""
    return SimpleDateFormat("MMM d · HH:mm", Locale.getDefault()).format(d)
}
